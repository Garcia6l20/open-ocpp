/*
Copyright (c) 2020 Cedric Jimenez
This file is part of OpenOCPP.

OpenOCPP is free software: you can redistribute it and/or modify
it under the terms of the GNU Lesser General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

OpenOCPP is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU Lesser General Public License for more details.

You should have received a copy of the GNU Lesser General Public License
along with OpenOCPP. If not, see <http://www.gnu.org/licenses/>.
*/

#include "StatusManager.h"
#include "BootNotification.h"
#include "Connectors.h"
#include "GenericMessageSender.h"
#include "Heartbeat.h"
#include "IChargePointConfig.h"
#include "IChargePointEventsHandler.h"
#include "IInternalConfigManager.h"
#include "IOcppConfig.h"
#include "InternalConfigKeys.h"
#include "Logger.h"
#include "StatusNotification.h"
#include "WorkerThreadPool.h"

#include <functional>
#include <thread>

using namespace ocpp::types;
using namespace ocpp::messages;

namespace ocpp
{
namespace chargepoint
{

/** @brief Constructor */
StatusManager::StatusManager(const ocpp::config::IChargePointConfig&         stack_config,
                             ocpp::config::IOcppConfig&                      ocpp_config,
                             IChargePointEventsHandler&                      events_handler,
                             ocpp::config::IInternalConfigManager&           internal_config,
                             ocpp::helpers::ITimerPool&                      timer_pool,
                             ocpp::helpers::WorkerThreadPool&                worker_pool,
                             Connectors&                                     connectors,
                             ocpp::messages::IMessageDispatcher&             msg_dispatcher,
                             ocpp::messages::GenericMessageSender&           msg_sender,
                             const ocpp::messages::GenericMessagesConverter& messages_converter,
                             ITriggerMessageManager&                         trigger_manager)
    : GenericMessageHandler<ChangeAvailabilityReq, ChangeAvailabilityConf>(CHANGE_AVAILABILITY_ACTION, messages_converter),

      m_stack_config(stack_config),
      m_ocpp_config(ocpp_config),
      m_events_handler(events_handler),
      m_internal_config(internal_config),
      m_worker_pool(worker_pool),
      m_connectors(connectors),
      m_msg_sender(msg_sender),
      m_registration_status(RegistrationStatus::Rejected),
      m_force_boot_notification(false),
      m_boot_notification_timer(timer_pool, "Boot notification"),
      m_heartbeat_timer(timer_pool, "Heartbeat")
{
    m_boot_notification_timer.setCallback(std::bind(&StatusManager::bootNotificationProcess, this));
    m_heartbeat_timer.setCallback(std::bind(&StatusManager::heartBeatProcess, this));

    trigger_manager.registerHandler(ocpp::types::MessageTrigger::BootNotification, *this);
    trigger_manager.registerHandler(ocpp::types::MessageTrigger::Heartbeat, *this);
    trigger_manager.registerHandler(ocpp::types::MessageTrigger::StatusNotification, *this);
    trigger_manager.registerHandler(ocpp::types::MessageTriggerEnumType::BootNotification, *this);
    trigger_manager.registerHandler(ocpp::types::MessageTriggerEnumType::Heartbeat, *this);
    trigger_manager.registerHandler(ocpp::types::MessageTriggerEnumType::StatusNotification, *this);

    msg_dispatcher.registerHandler(CHANGE_AVAILABILITY_ACTION, *this);
}

/** @brief Destructor */
StatusManager::~StatusManager() { }

/** @copydoc void IStatusManager::forceRegistrationStatus(ocpp::types::RegistrationStatus) */
void StatusManager::forceRegistrationStatus(ocpp::types::RegistrationStatus status)
{
    m_registration_status     = status;
    m_force_boot_notification = true;
}

/** @copydoc void IStatusManager::updateConnectionStatus(bool) */
void StatusManager::updateConnectionStatus(bool is_connected)
{
    if (is_connected)
    {
        // If not accepted by the central system, restart boot notification process
        if (m_force_boot_notification || (m_registration_status != RegistrationStatus::Accepted))
        {
            m_boot_notification_timer.start(std::chrono::milliseconds(1u), true);
        }
        else
        {
            // If the status of a connector has changed since the last notification
            // to the central system, send the new connector status
            for (const Connector* connector : m_connectors.getConnectors())
            {
                if (connector->status != connector->last_notified_status)
                {
                    statusNotificationProcess(connector->id);
                }
            }

            // Restart heartbeat process
            m_heartbeat_timer.start(m_heartbeat_timer.getInterval());
        }
    }
    else
    {
        // Stop boot notification and heartbeat processes
        m_boot_notification_timer.stop();
        m_heartbeat_timer.stop();
    }
}

/** @copydoc bool IStatusManager::updateConnectorStatus(unsigned int,
 *                                                      ocpp::types::ChargePointStatus,
 *                                                      ocpp::types::ChargePointErrorCode,
 *                                                      const std::string&,
 *                                                      const std::string&,
 *                                                      const std::string&)
*/
bool StatusManager::updateConnectorStatus(unsigned int                      connector_id,
                                          ocpp::types::ChargePointStatus    status,
                                          ocpp::types::ChargePointErrorCode error_code,
                                          const std::string&                info,
                                          const std::string&                vendor_id,
                                          const std::string&                vendor_error)
{
    bool ret = false;

    // Get selected connector
    Connector* connector = m_connectors.getConnector(connector_id);
    if (connector)
    {
        // Check if status has changed
        if (connector->status != status)
        {
            {
                std::lock_guard<std::mutex> lock(connector->mutex);

                // Save new status
                connector->status           = status;
                connector->status_timestamp = DateTime::now();
                connector->error_code       = error_code;
                connector->info             = info;
                connector->vendor_id        = vendor_id;
                connector->vendor_error     = vendor_error;
                m_connectors.saveConnector(connector->id);
            }

            LOG_INFO << "Connector " << connector_id << " : " << ChargePointStatusHelper.toString(status);

            // Check registration status
            if (m_registration_status == RegistrationStatus::Accepted)
            {
                // Check minimum status duration
                std::chrono::seconds duration = m_ocpp_config.minimumStatusDuration();
                if (duration == std::chrono::seconds(0))
                {
                    // Notify now
                    statusNotificationProcess(connector_id);
                }
                else
                {
                    // Notify later if needed
                    connector->status_timer.stop();
                    if (connector->status != connector->last_notified_status)
                    {
                        connector->status_timer.setCallback([connector_id, this] { statusNotificationProcess(connector_id); });
                        connector->status_timer.start(std::chrono::milliseconds(duration), true);
                    }
                }
            }
        }
        ret = true;
    }

    return ret;
}

/** @copydoc void IStatusManager::resetHeartBeatTimer() */
void StatusManager::resetHeartBeatTimer()
{
    if (m_heartbeat_timer.isStarted())
    {
        m_heartbeat_timer.restart(m_heartbeat_timer.getInterval());
    }
}

// ITriggerMessageHandler interfaces

/** @copydoc bool ITriggerMessageHandler::onTriggerMessage(ocpp::types::MessageTrigger, const ocpp::types::Optional<unsigned int>&) */
bool StatusManager::onTriggerMessage(ocpp::types::MessageTrigger message, const ocpp::types::Optional<unsigned int>& connector_id)
{
    bool ret = true;
    switch (message)
    {
        case MessageTrigger::BootNotification:
        {
            m_worker_pool.run<void>(
                [this]
                {
                    // To let some time for the trigger message reply
                    std::this_thread::sleep_for(std::chrono::milliseconds(250u));
                    sendBootNotification();
                });
        }
        break;

        case MessageTrigger::Heartbeat:
        {
            m_worker_pool.run<void>(
                [this]
                {
                    // To let some time for the trigger message reply
                    std::this_thread::sleep_for(std::chrono::milliseconds(250u));
                    heartBeatProcess();
                });
        }
        break;

        case MessageTrigger::StatusNotification:
        {
            if (connector_id.isSet())
            {
                unsigned int id = connector_id;
                m_worker_pool.run<void>(
                    [this, id]
                    {
                        // To let some time for the trigger message reply
                        std::this_thread::sleep_for(std::chrono::milliseconds(250u));
                        statusNotificationProcess(id);
                    });
            }
            else
            {
                for (const Connector* connector : m_connectors.getConnectors())
                {
                    unsigned int id = connector->id;
                    m_worker_pool.run<void>(
                        [this, id]
                        {
                            // To let some time for the trigger message reply
                            std::this_thread::sleep_for(std::chrono::milliseconds(250u));
                            statusNotificationProcess(id);
                        });
                }
            }
            break;
        }

        default:
        {
            // Unknown message
            ret = false;
            break;
        }
    }
    return ret;
}

/** @copydoc bool ITriggerMessageHandler::onTriggerMessage(ocpp::types::MessageTriggerEnumType, const ocpp::types::Optional<unsigned int>&) */
bool StatusManager::onTriggerMessage(ocpp::types::MessageTriggerEnumType message, const ocpp::types::Optional<unsigned int>& connector_id)
{
    bool ret = true;
    switch (message)
    {
        case MessageTriggerEnumType::BootNotification:
        {
            m_worker_pool.run<void>(
                [this]
                {
                    // To let some time for the trigger message reply
                    std::this_thread::sleep_for(std::chrono::milliseconds(250u));
                    sendBootNotification();
                });
        }
        break;

        case MessageTriggerEnumType::Heartbeat:
        {
            m_worker_pool.run<void>(
                [this]
                {
                    // To let some time for the trigger message reply
                    std::this_thread::sleep_for(std::chrono::milliseconds(250u));
                    heartBeatProcess();
                });
        }
        break;

        case MessageTriggerEnumType::StatusNotification:
        {
            if (connector_id.isSet())
            {
                unsigned int id = connector_id;
                m_worker_pool.run<void>(
                    [this, id]
                    {
                        // To let some time for the trigger message reply
                        std::this_thread::sleep_for(std::chrono::milliseconds(250u));
                        statusNotificationProcess(id);
                    });
            }
            else
            {
                for (const Connector* connector : m_connectors.getConnectors())
                {
                    unsigned int id = connector->id;
                    m_worker_pool.run<void>(
                        [this, id]
                        {
                            // To let some time for the trigger message reply
                            std::this_thread::sleep_for(std::chrono::milliseconds(250u));
                            statusNotificationProcess(id);
                        });
                }
            }
            break;
        }

        default:
        {
            // Unknown message
            ret = false;
            break;
        }
    }
    return ret;
}

// GenericMessageHandler interface

/** @copydoc bool GenericMessageHandler<RequestType, ResponseType>::handleMessage(const RequestType& request,
 *                                                                                ResponseType& response,
 *                                                                                const char*& error_code,
 *                                                                                std::string& error_message)
 */
bool StatusManager::handleMessage(const ocpp::messages::ChangeAvailabilityReq& request,
                                  ocpp::messages::ChangeAvailabilityConf&      response,
                                  const char*&                                 error_code,
                                  std::string&                                 error_message)
{
    bool ret = false;

    LOG_INFO << "Change availability requested : connectorId = " << request.connectorId;

    // Check connector id
    unsigned int connector_id = request.connectorId;
    if (m_connectors.isValid(connector_id))
    {
        // Notify request
        response.status = m_events_handler.changeAvailabilityRequested(connector_id, request.type);
        if (response.status == AvailabilityStatus::Accepted)
        {
            // Update status
            ChargePointStatus status = ChargePointStatus::Unavailable;
            if (request.type == AvailabilityType::Operative)
            {
                status = ChargePointStatus::Available;
            }
            m_worker_pool.run<void>([this, connector_id, status] { updateConnectorStatus(connector_id, status); });
            ret = true;
        }

        LOG_INFO << "Change availability " << AvailabilityStatusHelper.toString(response.status);
    }
    else
    {
        error_code    = ocpp::rpc::IRpc::RPC_ERROR_PROPERTY_CONSTRAINT_VIOLATION;
        error_message = "Invalid connector id";
    }
    return ret;
}

/** @brief Boot notification process thread */
void StatusManager::bootNotificationProcess()
{
    // Fill boot notification request
    BootNotificationReq boot_req;
    boot_req.chargeBoxSerialNumber.value().assign(m_stack_config.chargeBoxSerialNumber());
    boot_req.chargePointModel.assign(m_stack_config.chargePointModel());
    boot_req.chargePointSerialNumber.value().assign(m_stack_config.chargePointSerialNumber());
    boot_req.chargePointVendor.assign(m_stack_config.chargePointVendor());
    boot_req.firmwareVersion.value().assign(m_stack_config.firmwareVersion());
    boot_req.iccid.value().assign(m_stack_config.iccid());
    boot_req.imsi.value().assign(m_stack_config.imsi());
    boot_req.meterSerialNumber.value().assign(m_stack_config.meterSerialNumber());

    // Send BootNotificationRequest
    BootNotificationConf boot_conf;
    CallResult           result = m_msg_sender.call(BOOT_NOTIFICATION_ACTION, boot_req, boot_conf);
    if (result == CallResult::Ok)
    {
        m_registration_status = boot_conf.status;
        if (m_registration_status == RegistrationStatus::Accepted)
        {
            // Send first status notifications
            for (unsigned int id = 0; id <= m_connectors.getCount(); id++)
            {
                statusNotificationProcess(id);
            }

            // Configure hearbeat
            std::chrono::seconds interval(boot_conf.interval);
            m_ocpp_config.heartbeatInterval(interval);
            m_heartbeat_timer.start(std::chrono::milliseconds(interval));
        }
        else
        {
            // Schedule next retry
            m_boot_notification_timer.start(std::chrono::seconds(boot_conf.interval), true);
        }

        std::string registration_status = RegistrationStatusHelper.toString(m_registration_status);
        LOG_INFO << "Registration status : " << registration_status;

        // Save registration status
        m_force_boot_notification = false;
        m_internal_config.setKey(LAST_REGISTRATION_STATUS_KEY, registration_status);

        // Notify boot
        m_events_handler.bootNotification(m_registration_status, boot_conf.currentTime);
    }
    else
    {
        // Schedule next retry
        m_boot_notification_timer.start(m_stack_config.retryInterval(), true);
    }
}

/** @brief Heartbeat process */
void StatusManager::heartBeatProcess()
{
    HeartbeatReq  heartbeat_req;
    HeartbeatConf heartbeat_conf;
    CallResult    result = m_msg_sender.call(HEARTBEAT_ACTION, heartbeat_req, heartbeat_conf);
    if (result == CallResult::Ok)
    {
        LOG_INFO << "Heartbeat : " << heartbeat_conf.currentTime.str();

        m_events_handler.datetimeReceived(heartbeat_conf.currentTime);
    }
}

/** @brief Status notification process */
void StatusManager::statusNotificationProcess(unsigned int connector_id)
{
    // Get connector
    Connector* connector = m_connectors.getConnector(connector_id);
    if (connector)
    {
        // Send request
        StatusNotificationReq status_req;
        status_req.connectorId = connector->id;
        status_req.status      = connector->status;
        status_req.timestamp   = connector->status_timestamp;
        status_req.errorCode   = connector->error_code;
        if (!connector->info.empty())
        {
            status_req.info.value().assign(connector->info);
        }
        if (!connector->vendor_id.empty())
        {
            status_req.vendorId.value().assign(connector->vendor_id);
        }
        if (!connector->vendor_error.empty())
        {
            status_req.vendorErrorCode.value().assign(connector->vendor_error);
        }

        StatusNotificationConf status_conf;
        CallResult             result = m_msg_sender.call(STATUS_NOTIFICATION_ACTION, status_req, status_conf);
        if (result == CallResult::Ok)
        {
            // Update last notified status
            connector->last_notified_status = connector->status;
        }
    }
}

/** @brief Send the boot notification message */
void StatusManager::sendBootNotification()
{
    // Fill boot notification request
    BootNotificationReq boot_req;
    boot_req.chargeBoxSerialNumber.value().assign(m_stack_config.chargeBoxSerialNumber());
    boot_req.chargePointModel.assign(m_stack_config.chargePointModel());
    boot_req.chargePointSerialNumber.value().assign(m_stack_config.chargePointSerialNumber());
    boot_req.chargePointVendor.assign(m_stack_config.chargePointVendor());
    boot_req.firmwareVersion.value().assign(m_stack_config.firmwareVersion());
    boot_req.iccid.value().assign(m_stack_config.iccid());
    boot_req.imsi.value().assign(m_stack_config.imsi());
    boot_req.meterSerialNumber.value().assign(m_stack_config.meterSerialNumber());

    // Send BootNotificationRequest
    BootNotificationConf boot_conf;
    CallResult           result = m_msg_sender.call(BOOT_NOTIFICATION_ACTION, boot_req, boot_conf);
    if (result == CallResult::Ok)
    {
        // Save registration status
        m_registration_status = boot_conf.status;

        // Restart hearbeat timer
        std::chrono::seconds interval(boot_conf.interval);
        m_ocpp_config.heartbeatInterval(interval);
        m_heartbeat_timer.restart(std::chrono::milliseconds(interval));
    }

    return;
}

} // namespace chargepoint
} // namespace ocpp
