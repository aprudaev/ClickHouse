#include <iomanip>

#include <Poco/Net/NetException.h>

#include <common/ClickHouseRevision.h>

#include <DB/Common/Stopwatch.h>

#include <DB/Core/Progress.h>

#include <DB/IO/CompressedReadBuffer.h>
#include <DB/IO/CompressedWriteBuffer.h>
#include <DB/IO/ReadBufferFromPocoSocket.h>
#include <DB/IO/WriteBufferFromPocoSocket.h>

#include <DB/IO/copyData.h>

#include <DB/DataStreams/AsynchronousBlockInputStream.h>
#include <DB/DataStreams/NativeBlockInputStream.h>
#include <DB/DataStreams/NativeBlockOutputStream.h>
#include <DB/Interpreters/executeQuery.h>
#include <DB/Interpreters/Quota.h>

#include <DB/Storages/StorageMemory.h>

#include <DB/Common/ExternalTable.h>

#include "TCPHandler.h"

#include <DB/Common/NetException.h>

namespace DB
{

namespace ErrorCodes
{
	extern const int CLIENT_HAS_CONNECTED_TO_WRONG_PORT;
	extern const int UNKNOWN_DATABASE;
	extern const int UNKNOWN_EXCEPTION;
	extern const int UNKNOWN_PACKET_FROM_CLIENT;
	extern const int POCO_EXCEPTION;
	extern const int STD_EXCEPTION;
	extern const int SOCKET_TIMEOUT;
	extern const int UNEXPECTED_PACKET_FROM_CLIENT;
}


void TCPHandler::runImpl()
{
	connection_context = *server.global_context;
	connection_context.setSessionContext(connection_context);

	Settings global_settings = server.global_context->getSettings();

	socket().setReceiveTimeout(global_settings.receive_timeout);
	socket().setSendTimeout(global_settings.send_timeout);
	socket().setNoDelay(true);

	in = std::make_shared<ReadBufferFromPocoSocket>(socket());
	out = std::make_shared<WriteBufferFromPocoSocket>(socket());

	if (in->eof())
	{
		LOG_WARNING(log, "Client has not sent any data.");
		return;
	}

	try
	{
		receiveHello();
	}
	catch (const Exception & e) /// Typical for an incorrect username, password, or address.
	{
		if (e.code() == ErrorCodes::CLIENT_HAS_CONNECTED_TO_WRONG_PORT)
		{
			LOG_DEBUG(log, "Client has connected to wrong port.");
			return;
		}

		if (e.code() == ErrorCodes::ATTEMPT_TO_READ_AFTER_EOF)
		{
			LOG_WARNING(log, "Client has gone away.");
			return;
		}

		try
		{
		/// We try to send error information to the client.
			sendException(e);
		}
		catch (...) {}

		throw;
	}

	/// When connecting, the default database can be specified.
	if (!default_database.empty())
	{
		if (!connection_context.isDatabaseExist(default_database))
		{
			Exception e("Database " + default_database + " doesn't exist", ErrorCodes::UNKNOWN_DATABASE);
			LOG_ERROR(log, "Code: " << e.code() << ", e.displayText() = " << e.displayText()
				<< ", Stack trace:\n\n" << e.getStackTrace().toString());
			sendException(e);
			return;
		}

		connection_context.setCurrentDatabase(default_database);
	}

	sendHello();

	connection_context.setProgressCallback([this] (const Progress & value) { return this->updateProgress(value); });

	while (1)
	{
		/// We are waiting for package from client. Thus, every `POLL_INTERVAL` seconds check whether you do not need to complete the work.
		while (!static_cast<ReadBufferFromPocoSocket &>(*in).poll(global_settings.poll_interval * 1000000) && !BaseDaemon::instance().isCancelled())
			;

		/// If you need to quit, or client disconnects.
		if (BaseDaemon::instance().isCancelled() || in->eof())
			break;

		Stopwatch watch;
		state.reset();

		/** An exception during the execution of request (it must be sent over the network to the client).
		  * The client will be able to accept it, if it did not happen while sending another packet and the client has not disconnected yet.
		  */
		std::unique_ptr<Exception> exception;

		try
		{
		/// Restore context of request.
			query_context = connection_context;

		/** If Query - process it. If Ping or Cancel - go back to the beginning.
			  * There may come settings for a separate query that modify `query_context`.
			  */
			if (!receivePacket())
				continue;

		/// Get blocks of temporary tables
			if (client_revision >= DBMS_MIN_REVISION_WITH_TEMPORARY_TABLES)
				readData(global_settings);

		/// We are clearing, as we received an empty block from the external table data.
		/// So, stream is marked as cancelled and can not be read from it.
			state.block_in.reset();
		state.maybe_compressed_in.reset();  /// For more accurate accounting of MemoryTracker.

		/// Processing Query
			state.io = executeQuery(state.query, query_context, false, state.stage);

			if (state.io.out)
				state.need_receive_data_for_insert = true;

			after_check_cancelled.restart();
			after_send_progress.restart();

		/// Does the request require receive data from client?
			if (state.need_receive_data_for_insert)
				processInsertQuery(global_settings);
			else
				processOrdinaryQuery();

			sendEndOfStream();

			state.reset();
		}
		catch (const Exception & e)
		{
			state.io.onException();
			exception.reset(e.clone());

			if (e.code() == ErrorCodes::UNKNOWN_PACKET_FROM_CLIENT)
				throw;
		}
		catch (const Poco::Net::NetException & e)
		{
		/** We can get here if there was an error during connection to the client,
		  *  or in connection with a remote server that was used to process the request.
		  * It is not possible to distinguish between these two cases.
		  * Although in one of them, we have to send exception to the client, but in the other - we can not.
		  * We will try to send exception to the client in any case - see below.
			  */
			state.io.onException();
			exception = std::make_unique<Exception>(e.displayText(), ErrorCodes::POCO_EXCEPTION);
		}
		catch (const Poco::Exception & e)
		{
			state.io.onException();
			exception = std::make_unique<Exception>(e.displayText(), ErrorCodes::POCO_EXCEPTION);
		}
		catch (const std::exception & e)
		{
			state.io.onException();
			exception = std::make_unique<Exception>(e.what(), ErrorCodes::STD_EXCEPTION);
		}
		catch (...)
		{
			state.io.onException();
			exception = std::make_unique<Exception>("Unknown exception", ErrorCodes::UNKNOWN_EXCEPTION);
		}

		bool network_error = false;

		try
		{
			if (exception)
				sendException(*exception);
		}
		catch (...)
		{
		/** Could not send exception information to the client. */
			network_error = true;
			LOG_WARNING(log, "Client has gone away.");
		}

		try
		{
			state.reset();
		}
		catch (...)
		{
		/** During the processing of request, there was an exception that we caught and possibly sent to client.
		  * When destroying the request pipeline execution there was a second exception.
		  * For example, a pipeline could run in multiple threads, and an exception could occur in each of them.
			  * Ignore it.
			  */
		}

		watch.stop();

		LOG_INFO(log, std::fixed << std::setprecision(3)
			<< "Processed in " << watch.elapsedSeconds() << " sec.");

		if (network_error)
			break;
	}
}


void TCPHandler::readData(const Settings & global_settings)
{
	while (1)
	{
		Stopwatch watch(CLOCK_MONOTONIC_COARSE);

		/// We are waiting for package from the client. Thus, every `POLL_INTERVAL` seconds check whether you do not need to complete the work.
		while (1)
		{
			if (static_cast<ReadBufferFromPocoSocket &>(*in).poll(global_settings.poll_interval * 1000000))
				break;

		/// If you need to shut down work.
			if (BaseDaemon::instance().isCancelled())
				return;

		/** If we wait for data for too long.
		  * If we periodically poll, the receive_timeout of the socket itself does not work.
			  * Therefore, an additional check is added.
			  */
			if (watch.elapsedSeconds() > global_settings.receive_timeout.totalSeconds())
				throw Exception("Timeout exceeded while receiving data from client", ErrorCodes::SOCKET_TIMEOUT);
		}

		/// If client disconnected.
		if (in->eof())
			return;

		/// We accept and process data. And if they are over, then we leave.
		if (!receivePacket())
			break;
	}
}


void TCPHandler::processInsertQuery(const Settings & global_settings)
{
	/** Made above the rest of the lines, so that in case of `writePrefix` function throws an exception,
	  *  client receive exception before sending data.
	  */
	state.io.out->writePrefix();

	/// Send block to the client - table structure.
	Block block = state.io.out_sample;
	sendData(block);

	readData(global_settings);
	state.io.out->writeSuffix();
	state.io.onFinish();
}


void TCPHandler::processOrdinaryQuery()
{
	/// Pull query execution result, if exists, and send it to network.
	if (state.io.in)
	{
		/// Send header-block, to allow client to prepare output format for data to send.
		if (state.io.in_sample)
			sendData(state.io.in_sample);

		AsynchronousBlockInputStream async_in(state.io.in);
		async_in.readPrefix();

		while (true)
		{
			Block block;

			while (true)
			{
				if (isQueryCancelled())
				{
		/// A package was received requesting to stop execution of the request.
					async_in.cancel();
					break;
				}
				else
				{
					if (state.progress.rows && after_send_progress.elapsed() / 1000 >= query_context.getSettingsRef().interactive_delay)
					{
						/// Some time passed and there is a progress.
						after_send_progress.restart();
						sendProgress();
					}

					if (async_in.poll(query_context.getSettingsRef().interactive_delay / 1000))
					{
						/// There is the following result block.
						block = async_in.read();
						break;
					}
				}
			}

		/** If data has run out, we will send the profiling data and total values to
		  * the last zero block to be able to use
		  * this information in the suffix output of stream.
		  * If the request was interrupted, then `sendTotals` and other methods could not be called,
		  *  because we have not read all the data yet,
		  *  and there could be ongoing calculations in other threads at the same time.
			  */
			if (!block && !isQueryCancelled())
			{
				sendTotals();
				sendExtremes();
				sendProfileInfo();
				sendProgress();
			}

			sendData(block);
			if (!block)
				break;
		}

		async_in.readSuffix();
	}

	state.io.onFinish();
}


void TCPHandler::sendProfileInfo()
{
	if (const IProfilingBlockInputStream * input = dynamic_cast<const IProfilingBlockInputStream *>(&*state.io.in))
	{
		writeVarUInt(Protocol::Server::ProfileInfo, *out);
		input->getProfileInfo().write(*out);
		out->next();
	}
}


void TCPHandler::sendTotals()
{
	if (IProfilingBlockInputStream * input = dynamic_cast<IProfilingBlockInputStream *>(&*state.io.in))
	{
		const Block & totals = input->getTotals();

		if (totals)
		{
			initBlockOutput();

			writeVarUInt(Protocol::Server::Totals, *out);
			if (client_revision >= DBMS_MIN_REVISION_WITH_TEMPORARY_TABLES)
				writeStringBinary("", *out);

			state.block_out->write(totals);
			state.maybe_compressed_out->next();
			out->next();
		}
	}
}


void TCPHandler::sendExtremes()
{
	if (const IProfilingBlockInputStream * input = dynamic_cast<const IProfilingBlockInputStream *>(&*state.io.in))
	{
		const Block & extremes = input->getExtremes();

		if (extremes)
		{
			initBlockOutput();

			writeVarUInt(Protocol::Server::Extremes, *out);
			if (client_revision >= DBMS_MIN_REVISION_WITH_TEMPORARY_TABLES)
				writeStringBinary("", *out);

			state.block_out->write(extremes);
			state.maybe_compressed_out->next();
			out->next();
		}
	}
}


void TCPHandler::receiveHello()
{
	/// Receive `hello` packet.
	UInt64 packet_type = 0;
	String user = "default";
	String password;

	readVarUInt(packet_type, *in);
	if (packet_type != Protocol::Client::Hello)
	{
		/** If you accidentally accessed the HTTP protocol for a port destined for an internal TCP protocol,
		  * Then instead of the package number, there will be G (GET) or P (POST), in most cases.
		  */
		if (packet_type == 'G' || packet_type == 'P')
		{
			writeString("HTTP/1.0 400 Bad Request\r\n\r\n"
				"Port " + server.config().getString("tcp_port") + " is for clickhouse-client program.\r\n"
				"You must use port " + server.config().getString("http_port") + " for HTTP.\r\n",
				*out);

			throw Exception("Client has connected to wrong port", ErrorCodes::CLIENT_HAS_CONNECTED_TO_WRONG_PORT);
		}
		else
			throw NetException("Unexpected packet from client", ErrorCodes::UNEXPECTED_PACKET_FROM_CLIENT);
	}

	readStringBinary(client_name, *in);
	readVarUInt(client_version_major, *in);
	readVarUInt(client_version_minor, *in);
	readVarUInt(client_revision, *in);
	readStringBinary(default_database, *in);
	readStringBinary(user, *in);
	readStringBinary(password, *in);

	LOG_DEBUG(log, "Connected " << client_name
		<< " version " << client_version_major
		<< "." << client_version_minor
		<< "." << client_revision
		<< (!default_database.empty() ? ", database: " + default_database : "")
		<< (!user.empty() ? ", user: " + user : "")
		<< ".");

	connection_context.setUser(user, password, socket().peerAddress(), "");
}


void TCPHandler::sendHello()
{
	writeVarUInt(Protocol::Server::Hello, *out);
	writeStringBinary(DBMS_NAME, *out);
	writeVarUInt(DBMS_VERSION_MAJOR, *out);
	writeVarUInt(DBMS_VERSION_MINOR, *out);
	writeVarUInt(ClickHouseRevision::get(), *out);
	if (client_revision >= DBMS_MIN_REVISION_WITH_SERVER_TIMEZONE)
	{
		writeStringBinary(DateLUT::instance().getTimeZone(), *out);
	}
	out->next();
}


bool TCPHandler::receivePacket()
{
	UInt64 packet_type = 0;
	readVarUInt(packet_type, *in);

//	std::cerr << "Packet: " << packet_type << std::endl;

	switch (packet_type)
	{
		case Protocol::Client::Query:
			if (!state.empty())
				throw NetException("Unexpected packet Query received from client", ErrorCodes::UNEXPECTED_PACKET_FROM_CLIENT);
			receiveQuery();
			return true;

		case Protocol::Client::Data:
			if (state.empty())
				throw NetException("Unexpected packet Data received from client", ErrorCodes::UNEXPECTED_PACKET_FROM_CLIENT);
			return receiveData();

		case Protocol::Client::Ping:
			writeVarUInt(Protocol::Server::Pong, *out);
			out->next();
			return false;

		case Protocol::Client::Cancel:
			return false;

		case Protocol::Client::Hello:
			throw Exception("Unexpected packet " + String(Protocol::Client::toString(packet_type)) + " received from client",
				ErrorCodes::UNEXPECTED_PACKET_FROM_CLIENT);

		default:
			throw Exception("Unknown packet from client", ErrorCodes::UNKNOWN_PACKET_FROM_CLIENT);
	}
}


void TCPHandler::receiveQuery()
{
	UInt64 stage = 0;
	UInt64 compression = 0;

	state.is_empty = false;
	readStringBinary(state.query_id, *in);

	query_context.setCurrentQueryId(state.query_id);

	/// Client info
	{
		ClientInfo & client_info = query_context.getClientInfo();
		if (client_revision >= DBMS_MIN_REVISION_WITH_CLIENT_INFO)
			client_info.read(*in, client_revision);

		/// For better support of old clients, that does not send ClientInfo.
		if (client_info.query_kind == ClientInfo::QueryKind::NO_QUERY)
		{
			client_info.query_kind = ClientInfo::QueryKind::INITIAL_QUERY;
			client_info.client_name = client_name;
			client_info.client_version_major = client_version_major;
			client_info.client_version_minor = client_version_minor;
			client_info.client_revision = client_revision;
		}

		/// Set fields, that are known apriori.
		client_info.interface = ClientInfo::Interface::TCP;

		if (client_info.query_kind == ClientInfo::QueryKind::INITIAL_QUERY)
		{
			/// 'Current' fields was set at receiveHello.
			client_info.initial_user = client_info.current_user;
			client_info.initial_query_id = client_info.current_query_id;
			client_info.initial_address = client_info.current_address;
		}
	}

	/// Per query settings.
	query_context.getSettingsRef().deserialize(*in);

	readVarUInt(stage, *in);
	state.stage = QueryProcessingStage::Enum(stage);

	readVarUInt(compression, *in);
	state.compression = Protocol::Compression::Enum(compression);

	readStringBinary(state.query, *in);
}


bool TCPHandler::receiveData()
{
	initBlockInput();

	/// The name of the temporary table for writing data, default to empty string
	String external_table_name;
	if (client_revision >= DBMS_MIN_REVISION_WITH_TEMPORARY_TABLES)
		readStringBinary(external_table_name, *in);

	/// Read one block from the network and write it down
	Block block = state.block_in->read();

	if (block)
	{
		/// If there is an insert request, then the data should be written directly to `state.io.out`.
		/// Otherwise, we write the blocks in the temporary `external_table_name` table.
		if (!state.need_receive_data_for_insert)
		{
			StoragePtr storage;
			/// If such a table does not exist, create it.
			if (!(storage = query_context.tryGetExternalTable(external_table_name)))
			{
				NamesAndTypesListPtr columns = std::make_shared<NamesAndTypesList>(block.getColumnsList());
				storage = StorageMemory::create(external_table_name, columns);
				query_context.addExternalTable(external_table_name, storage);
			}
			/// The data will be written directly to the table.
			state.io.out = storage->write(ASTPtr(), query_context.getSettingsRef());
		}
		if (block)
			state.io.out->write(block);
		return true;
	}
	else
		return false;
}


void TCPHandler::initBlockInput()
{
	if (!state.block_in)
	{
		if (state.compression == Protocol::Compression::Enable)
			state.maybe_compressed_in = std::make_shared<CompressedReadBuffer>(*in);
		else
			state.maybe_compressed_in = in;

		state.block_in = std::make_shared<NativeBlockInputStream>(
			*state.maybe_compressed_in,
			client_revision);
	}
}


void TCPHandler::initBlockOutput()
{
	if (!state.block_out)
	{
		if (state.compression == Protocol::Compression::Enable)
			state.maybe_compressed_out = std::make_shared<CompressedWriteBuffer>(
				*out, query_context.getSettingsRef().network_compression_method);
		else
			state.maybe_compressed_out = out;

		state.block_out = std::make_shared<NativeBlockOutputStream>(
			*state.maybe_compressed_out,
			client_revision);
	}
}


bool TCPHandler::isQueryCancelled()
{
	if (state.is_cancelled || state.sent_all_data)
		return true;

	if (after_check_cancelled.elapsed() / 1000 < query_context.getSettingsRef().interactive_delay)
		return false;

	after_check_cancelled.restart();

	/// During request execution the only packet that can come from the client is stopping the query.
	if (static_cast<ReadBufferFromPocoSocket &>(*in).poll(0))
	{
		UInt64 packet_type = 0;
		readVarUInt(packet_type, *in);

		switch (packet_type)
		{
			case Protocol::Client::Cancel:
				if (state.empty())
					throw NetException("Unexpected packet Cancel received from client", ErrorCodes::UNEXPECTED_PACKET_FROM_CLIENT);
				LOG_INFO(log, "Query was cancelled.");
				state.is_cancelled = true;
				return true;

			default:
				throw NetException("Unknown packet from client", ErrorCodes::UNKNOWN_PACKET_FROM_CLIENT);
		}
	}

	return false;
}


void TCPHandler::sendData(Block & block)
{
	initBlockOutput();

	writeVarUInt(Protocol::Server::Data, *out);
	if (client_revision >= DBMS_MIN_REVISION_WITH_TEMPORARY_TABLES)
		writeStringBinary("", *out);

	state.block_out->write(block);
	state.maybe_compressed_out->next();
	out->next();
}


void TCPHandler::sendException(const Exception & e)
{
	writeVarUInt(Protocol::Server::Exception, *out);
	writeException(e, *out);
	out->next();
}


void TCPHandler::sendEndOfStream()
{
	state.sent_all_data = true;
	writeVarUInt(Protocol::Server::EndOfStream, *out);
	out->next();
}


void TCPHandler::updateProgress(const Progress & value)
{
	state.progress.incrementPiecewiseAtomically(value);
}


void TCPHandler::sendProgress()
{
	writeVarUInt(Protocol::Server::Progress, *out);
	Progress increment = state.progress.fetchAndResetPiecewiseAtomically();
	increment.write(*out, client_revision);
	out->next();
}


void TCPHandler::run()
{
	try
	{
		runImpl();

		LOG_INFO(log, "Done processing connection.");
	}
	catch (Poco::Exception & e)
	{
		/// Timeout - not an error.
		if (!strcmp(e.what(), "Timeout"))
		{
			LOG_DEBUG(log, "Poco::Exception. Code: " << ErrorCodes::POCO_EXCEPTION << ", e.code() = " << e.code()
				<< ", e.displayText() = " << e.displayText() << ", e.what() = " << e.what());
		}
		else
			throw;
	}
}


}
