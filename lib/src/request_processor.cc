#include <cthun-agent/request_processor.hpp>
#include <cthun-agent/action_outcome.hpp>
#include <cthun-agent/configuration.hpp>
#include <cthun-agent/file_utils.hpp>
#include <cthun-agent/rpc_schemas.hpp>
#include <cthun-agent/string_utils.hpp>
#include <cthun-agent/timer.hpp>
#include <cthun-agent/uuid.hpp>

#include <cthun-client/protocol/schemas.hpp>

#include <boost/filesystem.hpp>

#define LEATHERMAN_LOGGING_NAMESPACE "puppetlabs.cthun_agent.action_executer"
#include <leatherman/logging/logging.hpp>

#include <vector>
#include <atomic>
#include <functional>

namespace CthunAgent {

// Utility free function

std::vector<CthunClient::DataContainer> wrapDebug(
                        const CthunClient::ParsedChunks& parsed_chunks) {
    auto request_id = parsed_chunks.envelope.get<std::string>("id");
    if (parsed_chunks.num_invalid_debug) {
        LOG_WARNING("Message %1% contained %2% bad debug chunk%3%",
                    request_id, parsed_chunks.num_invalid_debug,
                    StringUtils::plural(parsed_chunks.num_invalid_debug));
    }
    std::vector<CthunClient::DataContainer> debug {};
    for (auto& debug_entry : parsed_chunks.debug) {
        debug.push_back(debug_entry);
    }
    return debug;
}

//
// Results Storage
//

class ResultsStorage {
  public:
    // Throw a file_error in case of failure while writing to any of
    // result files
    ResultsStorage(const ActionRequest& request, const std::string results_dir_)
            : module { request.module() },
              action { request.action() },
              out_path { results_dir_ + "/stdout" },
              err_path { results_dir_ + "/stderr" },
              status_path { results_dir_ + "/status" },
              action_status {} {
        initialize(request);
    }

    void write(const ActionOutcome& outcome, const std::string& exec_error,
               const std::string& duration) {
        action_status.set<std::string>("status", "completed");
        action_status.set<std::string>("duration", duration);
        FileUtils::writeToFile(action_status.toString() + "\n", status_path);

        if (exec_error.empty()) {
            if (outcome.type == ActionOutcome::Type::External) {
                FileUtils::writeToFile(outcome.stdout + "\n", out_path);
                if (!outcome.stderr.empty()) {
                    FileUtils::writeToFile(outcome.stderr + "\n", err_path);
                }
            } else {
                // ActionOutcome::Type::Internal
                FileUtils::writeToFile(outcome.results.toString() + "\n", out_path);
            }
        } else {
            std::string err_msg { "Failed to execute '" + module + " " + action
                                  + "': " + exec_error + "\n" };
            FileUtils::writeToFile(err_msg, err_path);
        }
    }

  private:
    std::string module;
    std::string action;
    std::string out_path;
    std::string err_path;
    std::string status_path;
    CthunClient::DataContainer action_status;

    void initialize(const ActionRequest& request) {
        action_status.set<std::string>("module", module);
        action_status.set<std::string>("action", action);
        action_status.set<std::string>("status", "running");
        action_status.set<std::string>("duration", "0 s");

        if (!request.paramsTxt().empty()) {
            action_status.set<std::string>("input", request.paramsTxt());
        } else {
            action_status.set<std::string>("input", "none");
        }

        FileUtils::writeToFile("", out_path);
        FileUtils::writeToFile("", err_path);
        FileUtils::writeToFile(action_status.toString() + "\n", status_path);
    }
};

//
// Non-blocking action task
//

void nonBlockingActionTask(std::shared_ptr<Module> module_ptr,
                           ActionRequest&& request,
                           std::string job_id,
                           ResultsStorage results_storage,
                           RequestProcessor& request_processor,
                           std::shared_ptr<std::atomic<bool>> done) {
    CthunAgent::Timer timer {};
    std::string exec_error {};
    ActionOutcome outcome {};

    try {
        outcome = module_ptr->executeAction(request);

        if (request.parsedChunks().data.get<bool>("notify_outcome")) {
            request_processor.replyNonBlockingResponse(request,
                                                       outcome.results,
                                                       job_id);
        }
    } catch (request_error& e) {
        exec_error = e.what();
        request_processor.replyRPCError(request, exec_error);
    }

    // Store results on disk
    auto duration = std::to_string(timer.elapsedSeconds()) + " s";
    results_storage.write(outcome, exec_error, duration);

    // Flag end of processing
    *done = true;
}

//
// Public interface
//

RequestProcessor::RequestProcessor(
                        std::shared_ptr<CthunClient::Connector> connector_ptr)
        : thread_container_ { "Action Executer" },
          connector_ptr_ { connector_ptr },
          spool_dir_ { Configuration::Instance().get<std::string>("spool-dir") } {
    if (!boost::filesystem::exists(spool_dir_)) {
        LOG_INFO("Creating spool directory '%1%'", spool_dir_);
        if (!FileUtils::createDirectory(spool_dir_)) {
            throw fatal_error { "failed to create the results directory '"
                                + spool_dir_ + "'" };
        }
    }
}

void RequestProcessor::replyCthunError(const std::string& request_id,
                                       const std::string& description,
                                       const std::vector<std::string>& endpoints) {
    CthunClient::DataContainer cthun_error_data {};
    cthun_error_data.set<std::string>("id", request_id);
    cthun_error_data.set<std::string>("description", description);

    try {
        connector_ptr_->send(endpoints,
                             CthunClient::Protocol::ERROR_MSG_TYPE,
                             DEFAULT_MSG_TIMEOUT_SEC,
                             cthun_error_data);
        LOG_INFO("Replied to request %1% with a Cthun core error message",
                 request_id);
    } catch (CthunClient::connection_error& e) {
        LOG_ERROR("Failed to send Cthun core error message for request %1%: %3%",
                  request_id, e.what());
    }
}

void RequestProcessor::replyRPCError(const ActionRequest& request,
                                     const std::string& description) {
    CthunClient::DataContainer rpc_error_data {};
    rpc_error_data.set<std::string>("transaction_id", request.transactionId());
    rpc_error_data.set<std::string>("id", request.id());
    rpc_error_data.set<std::string>("description", description);

    try {
        connector_ptr_->send(std::vector<std::string> { request.sender() },
                             RPCSchemas::RPC_ERROR_MSG_TYPE,
                             DEFAULT_MSG_TIMEOUT_SEC,
                             rpc_error_data);
        LOG_INFO("Replied to %1% request %2% by %3%, transaction %4%, with "
                 "an RPC error message", requestTypeNames[request.type()],
                 request.id(), request.sender(), request.transactionId());
    } catch (CthunClient::connection_error& e) {
        LOG_ERROR("Failed to send RPC error message for %1% request %2% by "
                  "%3%, transaction %4% (no further sending attempts): %5%",
                  requestTypeNames[request.type()], request.id(),
                  request.sender(), request.transactionId(), description);
    }
}

void RequestProcessor::replyBlockingResponse(
                            const ActionRequest& request,
                            const CthunClient::DataContainer& results) {
    auto debug = wrapDebug(request.parsedChunks());
    CthunClient::DataContainer response_data {};
    response_data.set<std::string>("transaction_id", request.transactionId());
    response_data.set<CthunClient::DataContainer>("results", results);

    try {
        connector_ptr_->send(std::vector<std::string> { request.sender() },
                             RPCSchemas::BLOCKING_RESPONSE_TYPE,
                             DEFAULT_MSG_TIMEOUT_SEC,
                             response_data,
                             debug);
    } catch (CthunClient::connection_error& e) {
        LOG_ERROR("Failed to reply to blocking request %1% from %2%, "
                  "transaction %3%: %4%", request.id(), request.sender(),
                  request.transactionId(), e.what());
    }
}

void RequestProcessor::replyNonBlockingResponse(
                            const ActionRequest& request,
                            const CthunClient::DataContainer& results,
                            const std::string& job_id) {
    CthunClient::DataContainer response_data {};
    response_data.set<std::string>("transaction_id", request.transactionId());
    response_data.set<std::string>("job_id", job_id);
    response_data.set<CthunClient::DataContainer>("results", results);

    try {
        // NOTE(ale): assuming debug was sent in provisional response
        connector_ptr_->send(std::vector<std::string> { request.sender() },
                            RPCSchemas::NON_BLOCKING_RESPONSE_TYPE,
                            DEFAULT_MSG_TIMEOUT_SEC,
                            response_data);
        LOG_INFO("Sent response for non-blocking request %1% by %2%, "
                 "transaction %3%", request.id(), request.sender(),
                 request.transactionId());
    } catch (CthunClient::connection_error& e) {
        LOG_ERROR("Failed to reply to non-blocking request %1% by %2%, "
                  "transaction %3% (no further attempts): %4%",
                  request.id(), request.sender(), request.transactionId(),
                  e.what());
    }
}

void RequestProcessor::replyProvisionalResponse(const ActionRequest& request,
                                                const std::string& job_id,
                                                const std::string& error) {
    auto debug = wrapDebug(request.parsedChunks());
    CthunClient::DataContainer provisional_data {};
    provisional_data.set<std::string>("transaction_id", request.transactionId());
    provisional_data.set<bool>("success", error.empty());
    provisional_data.set<std::string>("job_id", job_id);
    if (!error.empty()) {
        provisional_data.set<std::string>("error", error);
    }

    try {
        connector_ptr_->send(std::vector<std::string> { request.sender() },
                             RPCSchemas::PROVISIONAL_RESPONSE_TYPE,
                             DEFAULT_MSG_TIMEOUT_SEC,
                             provisional_data,
                             debug);
        LOG_INFO("Sent provisional response for request %1% by %2%, "
                 "transaction %3%", request.id(), request.sender(),
                 request.transactionId());
    } catch (CthunClient::connection_error& e) {
        LOG_ERROR("Failed to send provisional response for request %1% by "
                  "%2%, transaction %3% (no further attempts): %4%",
                  request.id(), request.sender(), request.transactionId(), e.what());
    }
}

void RequestProcessor::processRequest(std::shared_ptr<Module> module_ptr,
                                      const ActionRequest& request) {
    try {
        if (request.type() == RequestType::Blocking) {
            processBlockingRequest(module_ptr, request);
        } else {
            processNonBlockingRequest(module_ptr, request);
        }
    } catch (request_error& e) {
        // Process failure; send an *RPC error*
        LOG_ERROR("Failed to process %1% request %2% by %3%, transaction %4%: "
                  "%5%", requestTypeNames[request.type()], request.id(),
                  request.sender(), request.transactionId(), e.what());
        replyRPCError(request, e.what());
    }
}

//
// Private interface
//

void RequestProcessor::processBlockingRequest(std::shared_ptr<Module> module_ptr,
                                              const ActionRequest& request) {
    // Execute action; possible request errors will be propagated
    auto outcome = module_ptr->executeAction(request);

    replyBlockingResponse(request, outcome.results);
}

void RequestProcessor::processNonBlockingRequest(std::shared_ptr<Module> module_ptr,
                                                 const ActionRequest& request) {
    auto job_id = UUID::getUUID();

    // HERE(ale): assuming spool_dir ends with '/' (up to Configuration)
    std::string results_dir { spool_dir_ + job_id };

    if (!boost::filesystem::exists(results_dir)) {
        LOG_DEBUG("Creating results directory for the '%1% %2%' job with ID %3% "
                  "for request transaction %4%", request.module(),
                  request.action(), job_id, request.transactionId());
        if (!FileUtils::createDirectory(results_dir)) {
            throw request_processing_error { "failed to create directory '"
                                             + results_dir + "'" };
        }
    }

    // Spawn action task
    LOG_DEBUG("Starting '%1% %2%' job with ID %3% for non-blocking request %4% "
              "by %5%, transaction %6%", request.module(), request.action(),
              job_id, request.id(), request.sender(), request.transactionId());
    std::string err_msg {};

    try {
        // Flag to enable signaling from task to thread_container
        auto done = std::make_shared<std::atomic<bool>>(false);

        thread_container_.add(std::thread(&nonBlockingActionTask,
                                          module_ptr,
                                          request,
                                          job_id,
                                          ResultsStorage { request, results_dir },
                                          std::ref(*this),
                                          done),
                              done);
    } catch (file_error& e) {
        // Failed to instantiate ResultsStorage
        LOG_ERROR("Failed to initialize result files for  '%1% %2%' action job "
                  "with ID %3%: %4%", request.module(), request.action(),
                  job_id, e.what());
        err_msg = std::string { "failed to initialize result files: " } + e.what();
    } catch (std::exception& e) {
        LOG_ERROR("Failed to spawn '%1% %2%' action job with ID %3%: %4%",
                  request.module(), request.action(), job_id, e.what());
        err_msg = std::string { "failed to start action task: " } + e.what();
    }

    // Send back provisional data
    replyProvisionalResponse(request, job_id, err_msg);
}

}  // namespace CthunAgent
