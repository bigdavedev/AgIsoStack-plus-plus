#include "isobus/isobus/isobus_task_controller_client.hpp"

#include "isobus/isobus/can_general_parameter_group_numbers.hpp"
#include "isobus/isobus/can_network_manager.hpp"
#include "isobus/isobus/can_stack_logger.hpp"
#include "isobus/isobus/isobus_virtual_terminal_client.hpp"
#include "isobus/utility/system_timing.hpp"
#include "isobus/utility/to_string.hpp"

#include <array>
#include <cassert>
#include <cstring>
#include <thread>

namespace isobus
{
	TaskControllerClient::TaskControllerClient(std::shared_ptr<PartneredControlFunction> partner, std::shared_ptr<InternalControlFunction> clientSource, std::shared_ptr<VirtualTerminalClient> primaryVT) :
	  languageCommandInterface(clientSource, partner),
	  partnerControlFunction(partner),
	  myControlFunction(clientSource),
	  primaryVirtualTerminal(primaryVT)
	{
	}

	TaskControllerClient::~TaskControllerClient()
	{
		terminate();
	}

	void TaskControllerClient::initialize(bool spawnThread)
	{
		// You cannot use this interface without having valid control functions.
		assert(nullptr != myControlFunction);
		assert(nullptr != partnerControlFunction);

		partnerControlFunction->add_parameter_group_number_callback(static_cast<std::uint32_t>(CANLibParameterGroupNumber::ProcessData), process_rx_message, this);
		partnerControlFunction->add_parameter_group_number_callback(static_cast<std::uint32_t>(CANLibParameterGroupNumber::Acknowledge), process_rx_message, this);
		CANNetworkManager::CANNetwork.add_global_parameter_group_number_callback(static_cast<std::uint32_t>(CANLibParameterGroupNumber::ProcessData), process_rx_message, this);

		if (!languageCommandInterface.get_initialized())
		{
			languageCommandInterface.initialize();
		}

		if (shouldTerminate)
		{
			shouldTerminate = false;
			initialized = false;
		}

		if (!initialized)
		{
			if (spawnThread)
			{
				workerThread = new std::thread([this]() { worker_thread_function(); });
			}
			initialized = true;
		}
	}

	void TaskControllerClient::configure(std::shared_ptr<DeviceDescriptorObjectPool> DDOP,
	                                     std::uint8_t maxNumberBoomsSupported,
	                                     std::uint8_t maxNumberSectionsSupported,
	                                     std::uint8_t maxNumberChannelsSupportedForPositionBasedControl,
	                                     bool reportToTCSupportsDocumentation,
	                                     bool reportToTCSupportsTCGEOWithoutPositionBasedControl,
	                                     bool reportToTCSupportsTCGEOWithPositionBasedControl,
	                                     bool reportToTCSupportsPeerControlAssignment,
	                                     bool reportToTCSupportsImplementSectionControl)
	{
		if (StateMachineState::Disconnected == get_state())
		{
			assert(nullptr != DDOP); // Client will not work without a DDOP.
			binaryDDOP.clear();
			clientDDOP = DDOP;
			numberBoomsSupported = maxNumberBoomsSupported;
			numberSectionsSupported = maxNumberSectionsSupported;
			numberChannelsSupportedForPositionBasedControl = maxNumberChannelsSupportedForPositionBasedControl;
			supportsDocumentation = reportToTCSupportsDocumentation;
			supportsTCGEOWithoutPositionBasedControl = reportToTCSupportsTCGEOWithoutPositionBasedControl;
			supportsTCGEOWithPositionBasedControl = reportToTCSupportsTCGEOWithPositionBasedControl;
			supportsPeerControlAssignment = reportToTCSupportsPeerControlAssignment;
			supportsImplementSectionControl = reportToTCSupportsImplementSectionControl;
		}
		else
		{
			// We don't want someone to erase our object pool or something while it is being used.
			CANStackLogger::error("[TC]: Cannot reconfigure TC client while it is running!");
		}
	}

	void TaskControllerClient::terminate()
	{
		if (initialized)
		{
			if (nullptr != partnerControlFunction)
			{
				partnerControlFunction->remove_parameter_group_number_callback(static_cast<std::uint32_t>(CANLibParameterGroupNumber::ProcessData), process_rx_message, this);
				partnerControlFunction->remove_parameter_group_number_callback(static_cast<std::uint32_t>(CANLibParameterGroupNumber::Acknowledge), process_rx_message, this);
				CANNetworkManager::CANNetwork.remove_global_parameter_group_number_callback(static_cast<std::uint32_t>(CANLibParameterGroupNumber::ProcessData), process_rx_message, this);
			}

			shouldTerminate = true;

			//if (nullptr != workerThread)
			//{
			//	workerThread->join();
			//	delete workerThread;
			//	workerThread = nullptr;
			//}
		}
	}

	std::uint8_t TaskControllerClient::get_number_booms_supported() const
	{
		return numberBoomsSupported;
	}

	std::uint8_t TaskControllerClient::get_number_sections_supported() const
	{
		return numberSectionsSupported;
	}

	std::uint8_t TaskControllerClient::get_number_channels_supported_for_position_based_control() const
	{
		return numberChannelsSupportedForPositionBasedControl;
	}

	bool TaskControllerClient::get_supports_documentation() const
	{
		return supportsDocumentation;
	}

	bool TaskControllerClient::get_supports_tcgeo_without_position_based_control() const
	{
		return supportsTCGEOWithoutPositionBasedControl;
	}

	bool TaskControllerClient::get_supports_tcgeo_with_position_based_control() const
	{
		return supportsTCGEOWithPositionBasedControl;
	}

	bool TaskControllerClient::get_supports_peer_control_assignment() const
	{
		return supportsPeerControlAssignment;
	}

	bool TaskControllerClient::get_supports_implement_section_control() const
	{
		return supportsImplementSectionControl;
	}

	bool TaskControllerClient::get_is_initialized() const
	{
		return initialized;
	}

	bool TaskControllerClient::get_is_connected() const
	{
		return (StateMachineState::Connected == currentState);
	}

	bool TaskControllerClient::get_is_task_active() const
	{
		return (get_is_connected() && (0 != (0x01 & tcStatusBitfield)));
	}

	void TaskControllerClient::update()
	{
		switch (currentState)
		{
			case StateMachineState::Disconnected:
			{
				enableStatusMessage = false;

				if (nullptr != clientDDOP)
				{
					set_state(StateMachineState::WaitForStartUpDelay);
				}
			}
			break;

			case StateMachineState::WaitForStartUpDelay:
			{
				if (SystemTiming::time_expired_ms(stateMachineTimestamp_ms, SIX_SECOND_TIMEOUT_MS))
				{
					CANStackLogger::debug("[TC]: Startup delay complete, waiting for TC server status message.");
					set_state(StateMachineState::WaitForServerStatusMessage);
				}
			}
			break;

			case StateMachineState::WaitForServerStatusMessage:
			{
				if (SystemTiming::time_expired_ms(stateMachineTimestamp_ms, SIX_SECOND_TIMEOUT_MS))
				{
					CANStackLogger::error("[TC]: Timeout waiting for TC status message. Resetting client connection.");
					set_state(StateMachineState::Disconnected);
				}
			}
			break;

			case StateMachineState::SendWorkingSetMaster:
			{
				if (send_working_set_master())
				{
					set_state(StateMachineState::SendStatusMessage);
				}
				else if (SystemTiming::time_expired_ms(stateMachineTimestamp_ms, TWO_SECOND_TIMEOUT_MS))
				{
					CANStackLogger::error("[TC]: Timeout sending working set master message. Resetting client connection.");
					set_state(StateMachineState::Disconnected);
				}
			}
			break;

			case StateMachineState::SendStatusMessage:
			{
				// Start sending the status message
				if (send_status())
				{
					enableStatusMessage = true;
					statusMessageTimestamp_ms = SystemTiming::get_timestamp_ms();
					set_state(StateMachineState::RequestVersion);
				}
				else if (SystemTiming::time_expired_ms(stateMachineTimestamp_ms, TWO_SECOND_TIMEOUT_MS))
				{
					CANStackLogger::error("[TC]: Timeout sending first status message. Resetting client connection.");
					set_state(StateMachineState::Disconnected);
				}
			}
			break;

			case StateMachineState::RequestVersion:
			{
				if (send_version_request())
				{
					set_state(StateMachineState::WaitForRequestVersionResponse);
				}
				else if (SystemTiming::time_expired_ms(stateMachineTimestamp_ms, TWO_SECOND_TIMEOUT_MS))
				{
					CANStackLogger::error("[TC]: Timeout sending version request message. Resetting client connection.");
					set_state(StateMachineState::Disconnected);
				}
			}
			break;

			case StateMachineState::WaitForRequestVersionResponse:
			{
				if (SystemTiming::time_expired_ms(stateMachineTimestamp_ms, TWO_SECOND_TIMEOUT_MS))
				{
					CANStackLogger::error("[TC]: Timeout waiting for version request response. Resetting client connection.");
					set_state(StateMachineState::Disconnected);
				}
			}
			break;

			case StateMachineState::WaitForRequestVersionFromServer:
			{
				if (SystemTiming::time_expired_ms(stateMachineTimestamp_ms, SIX_SECOND_TIMEOUT_MS))
				{
					CANStackLogger::warn("[TC]: Timeout waiting for version request from TC. This is not required, so proceeding anways.");
					set_state(StateMachineState::RequestLanguage);
				}
			}
			break;

			case StateMachineState::SendRequestVersionResponse:
			{
				if (send_request_version_response())
				{
					set_state(StateMachineState::RequestLanguage);
				}
				else if (SystemTiming::time_expired_ms(stateMachineTimestamp_ms, TWO_SECOND_TIMEOUT_MS))
				{
					CANStackLogger::error("[TC]: Timeout sending version request response. Resetting client connection.");
					set_state(StateMachineState::Disconnected);
				}
			}
			break;

			case StateMachineState::RequestLanguage:
			{
				if (languageCommandInterface.send_request_language_command())
				{
					set_state(StateMachineState::WaitForLanguageResponse);
				}
				else if (SystemTiming::time_expired_ms(stateMachineTimestamp_ms, SIX_SECOND_TIMEOUT_MS))
				{
					CANStackLogger::error("[TC]: Timeout trying to send request for language command message. Resetting client connection.");
					set_state(StateMachineState::Disconnected);
				}
			}
			break;

			case StateMachineState::WaitForLanguageResponse:
			{
				if ((SystemTiming::get_time_elapsed_ms(languageCommandInterface.get_language_command_timestamp()) < SIX_SECOND_TIMEOUT_MS) &&
				    ("" != languageCommandInterface.get_language_code()))
				{
					set_state(StateMachineState::ProcessDDOP);
				}
			}
			break;

			case StateMachineState::ProcessDDOP:
			{
				assert(0 != clientDDOP->size()); // Need to have a valid object pool!
				if (0 == binaryDDOP.size())
				{
					// Binary DDOP has not been generated before.
					if (clientDDOP->generate_binary_object_pool(binaryDDOP))
					{
						CANStackLogger::debug("[TC]: DDOP Generated, size: " + isobus::to_string(static_cast<int>(binaryDDOP.size())));
						set_state(StateMachineState::RequestStructureLabel);
					}
					else
					{
						CANStackLogger::error("[TC]: Cannot proceed with connection to TC due to invalid DDOP. Check log for [DDOP] events. TC client will now terminate.");
						this->terminate();
					}
				}
			}
			break;

			case StateMachineState::RequestStructureLabel:
			{
				if (send_request_structure_label())
				{
					set_state(StateMachineState::WaitForStructureLabelResponse);
				}
				else if (SystemTiming::time_expired_ms(stateMachineTimestamp_ms, TWO_SECOND_TIMEOUT_MS))
				{
					CANStackLogger::error("[TC]: Timeout trying to send request for TC structure label. Resetting client connection.");
					set_state(StateMachineState::Disconnected);
				}
			}
			break;

			case StateMachineState::WaitForStructureLabelResponse:
			{
				if (SystemTiming::time_expired_ms(stateMachineTimestamp_ms, TWO_SECOND_TIMEOUT_MS))
				{
					CANStackLogger::error("[TC]: Timeout waiting for TC structure label. Resetting client connection.");
					set_state(StateMachineState::Disconnected);
				}
			}
			break;

			case StateMachineState::RequestLocalizationLabel:
			{
				if (send_request_localization_label())
				{
					set_state(StateMachineState::WaitForLocalizationLabelResponse);
				}
				else if (SystemTiming::time_expired_ms(stateMachineTimestamp_ms, TWO_SECOND_TIMEOUT_MS))
				{
					CANStackLogger::error("[TC]: Timeout trying to send request for TC localization label. Resetting client connection.");
					set_state(StateMachineState::Disconnected);
				}
			}
			break;

			case StateMachineState::WaitForLocalizationLabelResponse:
			{
				if (SystemTiming::time_expired_ms(stateMachineTimestamp_ms, TWO_SECOND_TIMEOUT_MS))
				{
					CANStackLogger::error("[TC]: Timeout waiting for TC localization label. Resetting client connection.");
					set_state(StateMachineState::Disconnected);
				}
			}
			break;

			case StateMachineState::SendDeleteObjectPool:
			{
				if (send_delete_object_pool())
				{
					set_state(StateMachineState::WaitForDeleteObjectPoolResponse);
				}
				else if (SystemTiming::time_expired_ms(stateMachineTimestamp_ms, TWO_SECOND_TIMEOUT_MS))
				{
					CANStackLogger::error("[TC]: Timeout trying to send delete object pool message. Resetting client connection.");
					set_state(StateMachineState::Disconnected);
				}
			}
			break;

			case StateMachineState::WaitForDeleteObjectPoolResponse:
			{
				if (SystemTiming::time_expired_ms(stateMachineTimestamp_ms, TWO_SECOND_TIMEOUT_MS))
				{
					CANStackLogger::error("[TC]: Timeout waiting for delete object pool response. Resetting client connection.");
					set_state(StateMachineState::Disconnected);
				}
			}
			break;

			case StateMachineState::SendRequestTransferObjectPool:
			{
				if (send_request_object_pool_transfer())
				{
					set_state(StateMachineState::WaitForRequestTransferObjectPoolResponse);
				}
				else if (SystemTiming::time_expired_ms(stateMachineTimestamp_ms, TWO_SECOND_TIMEOUT_MS))
				{
					CANStackLogger::error("[TC]: Timeout trying to send request to transfer object pool. Resetting client connection.");
					set_state(StateMachineState::Disconnected);
				}
			}
			break;

			case StateMachineState::BeginTransferDDOP:
			{
				if (CANNetworkManager::CANNetwork.send_can_message(static_cast<std::uint32_t>(CANLibParameterGroupNumber::ECUtoVirtualTerminal),
				                                                   nullptr,
				                                                   binaryDDOP.size() + 1, // Account for Mux byte
				                                                   myControlFunction.get(),
				                                                   partnerControlFunction.get(),
				                                                   CANIdentifier::CANPriority::PriorityLowest7,
				                                                   process_tx_callback,
				                                                   this,
				                                                   process_internal_object_pool_upload_callback))
				{
					set_state(StateMachineState::WaitForDDOPTransfer);
				}
				else if (SystemTiming::time_expired_ms(stateMachineTimestamp_ms, TWO_SECOND_TIMEOUT_MS))
				{
					CANStackLogger::error("[TC]: Timeout trying to begin the object pool upload. Resetting client connection.");
					set_state(StateMachineState::Disconnected);
				}
			}
			break;

			case StateMachineState::WaitForDDOPTransfer:
			{
				// Waiting...
			}
			break;

			case StateMachineState::SendObjectPoolActivate:
			{
				if (send_object_pool_activate())
				{
					set_state(StateMachineState::WaitForObjectPoolActivateResponse);
				}
				else if (SystemTiming::time_expired_ms(stateMachineTimestamp_ms, TWO_SECOND_TIMEOUT_MS))
				{
					CANStackLogger::error("[TC]: Timeout trying to activate object pool. Resetting client connection.");
					set_state(StateMachineState::Disconnected);
				}
			}
			break;

			case StateMachineState::WaitForObjectPoolActivateResponse:
			{
				if (SystemTiming::time_expired_ms(stateMachineTimestamp_ms, TWO_SECOND_TIMEOUT_MS))
				{
					CANStackLogger::error("[TC]: Timeout waiting for activate object pool response. Resetting client connection.");
					set_state(StateMachineState::Disconnected);
				}
			}
			break;

			case StateMachineState::Connected:
			{
				if (SystemTiming::time_expired_ms(serverStatusMessageTimestamp_ms, SIX_SECOND_TIMEOUT_MS))
				{
					CANStackLogger::error("[TC]: Server Status Message Timeout. The TC may be offline.");
					set_state(StateMachineState::Disconnected);
				}
			}
			break;

			case StateMachineState::DeactivateObjectPool:
			{
				if (send_object_pool_deactivate())
				{
					set_state(StateMachineState::WaitForObjectPoolDeactivateResponse);
				}
				else if (SystemTiming::time_expired_ms(stateMachineTimestamp_ms, TWO_SECOND_TIMEOUT_MS))
				{
					CANStackLogger::error("[TC]: Timeout sending object pool deactivate. Client terminated.");
					set_state(StateMachineState::Disconnected);
					terminate();
				}
			}
			break;

			case StateMachineState::WaitForObjectPoolDeactivateResponse:
			{
				if (SystemTiming::time_expired_ms(stateMachineTimestamp_ms, TWO_SECOND_TIMEOUT_MS))
				{
					CANStackLogger::error("[TC]: Timeout waiting for deactivate object pool response. Client terminated.");
					set_state(StateMachineState::Disconnected);
					terminate();
				}
			}
			break;

			default:
			{
				assert(false); // Unknown state? File a bug on GitHub if you see this happen.
			}
			break;
		}

		if ((enableStatusMessage) &&
		    (SystemTiming::time_expired_ms(statusMessageTimestamp_ms, TWO_SECOND_TIMEOUT_MS)) &&
		    (send_status()))
		{
			statusMessageTimestamp_ms = SystemTiming::get_timestamp_ms();
		}
	}

	void TaskControllerClient::process_rx_message(CANMessage *message, void *parentPointer)
	{
		if ((nullptr != message) &&
		    (nullptr != parentPointer) &&
		    (CAN_DATA_LENGTH <= message->get_data_length()))
		{
			TaskControllerClient *parentTC = static_cast<TaskControllerClient *>(parentPointer);
			std::vector<std::uint8_t> &messageData = message->get_data();

			switch (message->get_identifier().get_parameter_group_number())
			{
				case static_cast<std::uint32_t>(CANLibParameterGroupNumber::Acknowledge):
				{
					if (AcknowledgementType::Negative == static_cast<AcknowledgementType>(message->get_uint8_at(0)))
					{
						std::uint32_t targetParameterGroupNumber = message->get_uint24_at(5);
						if (static_cast<std::uint32_t>(CANLibParameterGroupNumber::ProcessData) == targetParameterGroupNumber)
						{
							CANStackLogger::CAN_stack_log(CANStackLogger::LoggingLevel::Error, "[TC]: The TC Server is NACK-ing our messages. Disconnecting.");
							parentTC->set_state(StateMachineState::Disconnected);
						}
					}
				}
				break;

				case static_cast<std::uint32_t>(CANLibParameterGroupNumber::ProcessData):
				{
					switch (static_cast<ProcessDataCommands>(messageData[0] & 0x0F))
					{
						case ProcessDataCommands::TechnicalCapabilities:
						{
							switch (static_cast<TechnicalDataMessageCommands>(messageData[0] >> 4))
							{
								case TechnicalDataMessageCommands::ParameterVersion:
								{
									parentTC->serverVersion = messageData[1];
									parentTC->maxServerBootTime_s = messageData[2];
									parentTC->serverOptionsByte1 = messageData[3];
									parentTC->serverOptionsByte2 = messageData[4];
									parentTC->serverNumberOfBoomsForSectionControl = messageData[5];
									parentTC->serverNumberOfSectionsForSectionControl = messageData[6];
									parentTC->serverNumberOfChannelsForPositionBasedControl = messageData[7];

									if (messageData[1] > static_cast<std::uint8_t>(Version::SecondPublishedEdition))
									{
										CANStackLogger::warn("[TC]: Server version is newer than client's maximum supported version.");
									}
									CANStackLogger::debug("[TC]: TC Server supports " +
									                      isobus::to_string(static_cast<int>(messageData[5])) +
									                      " booms, " +
									                      isobus::to_string(static_cast<int>(messageData[6])) +
									                      " sections, and " +
									                      isobus::to_string(static_cast<int>(messageData[7])) +
									                      " position based control channels.");

									if (StateMachineState::WaitForRequestVersionResponse == parentTC->get_state())
									{
										parentTC->set_state(StateMachineState::WaitForRequestVersionFromServer);
									}
								}
								break;

								default:
								{
									CANStackLogger::warn("[TC]: Unsupported process data technical data message received. Message will be dropped.");
								}
								break;
							}
						}
						break;

						case ProcessDataCommands::DeviceDescriptor:
						{
							switch (static_cast<DeviceDescriptorCommands>(messageData[0] >> 4))
							{
								case DeviceDescriptorCommands::StructureLabel:
								{
									if (StateMachineState::WaitForStructureLabelResponse == parentTC->get_state())
									{
										if ((0xFF == messageData[1]) &&
										    (0xFF == messageData[2]) &&
										    (0xFF == messageData[3]) &&
										    (0xFF == messageData[4]) &&
										    (0xFF == messageData[5]) &&
										    (0xFF == messageData[6]) &&
										    (0xFF == messageData[7]) &&
										    (CAN_DATA_LENGTH == messageData.size()))
										{
											// TC has no structure label for us. Need to upload the DDOP.
											parentTC->set_state(StateMachineState::SendRequestTransferObjectPool);
										}
										else
										{
											std::string tcStructure;

											for (std::size_t i = 1; i < messageData.size(); i++)
											{
												tcStructure.push_back(messageData[i]);
											}

											if (tcStructure.size() > 40)
											{
												CANStackLogger::warn("[TC]: Structure Label from TC exceeds the max length allowed by ISO11783-10");
											}
											task_controller_object::Object *deviceObject = parentTC->clientDDOP->get_object_by_id(0);
											// Does your DDOP have a device object? Device object 0 is required by ISO11783-10
											assert(nullptr != deviceObject);
											assert(task_controller_object::ObjectTypes::Device == deviceObject->get_object_type());
											if (reinterpret_cast<task_controller_object::DeviceObject *>(deviceObject)->get_structure_label() == tcStructure)
											{
												// Structure label matched. No upload needed yet.
												CANStackLogger::debug("[TC]: Task controller structure labels match");
												parentTC->set_state(StateMachineState::RequestLocalizationLabel);
											}
											else
											{
												// Structure label did not match. Need to delete current DDOP and re-upload.
												CANStackLogger::info("[TC]: Task controller structure labels do not match. DDOP will be deleted and reuploaded.");
												parentTC->set_state(StateMachineState::SendDeleteObjectPool);
											}
										}
									}
									else
									{
										CANStackLogger::warn("[TC]: Structure label message received, but ignored due to current state machine state.");
									}
								}
								break;

								case DeviceDescriptorCommands::LocalizationLabel:
								{
									// Right now, we'll just reload the pool if the localization doesn't match, but
									// in the future we should permit modifications to the localization and DVP objects
									//! @todo Localization label partial pool handling
									if (StateMachineState::WaitForStructureLabelResponse == parentTC->get_state())
									{
										if ((0xFF == messageData[1]) &&
										    (0xFF == messageData[2]) &&
										    (0xFF == messageData[3]) &&
										    (0xFF == messageData[4]) &&
										    (0xFF == messageData[5]) &&
										    (0xFF == messageData[6]) &&
										    (0xFF == messageData[7]) &&
										    (CAN_DATA_LENGTH == messageData.size()))
										{
											// TC has no localization label for us. Need to upload the DDOP.
											parentTC->set_state(StateMachineState::SendRequestTransferObjectPool);
										}
										else
										{
											task_controller_object::Object *deviceObject = parentTC->clientDDOP->get_object_by_id(0);
											// Does your DDOP have a device object? Device object 0 is required by ISO11783-10
											assert(nullptr != deviceObject);
											assert(task_controller_object::ObjectTypes::Device == deviceObject->get_object_type());

											auto ddopLabel = reinterpret_cast<task_controller_object::DeviceObject *>(deviceObject)->get_localization_label();
											bool labelsMatch = true;

											for (std::uint_fast8_t i = 0; i < (CAN_DATA_LENGTH - 1); i++)
											{
												if (messageData[i] != ddopLabel[i])
												{
													labelsMatch = false;
													break;
												}
											}

											if (labelsMatch)
											{
												// DDOP labels all matched
												CANStackLogger::debug("[TC]: Task controller localization labels match");
												parentTC->set_state(StateMachineState::SendObjectPoolActivate);
											}
											else
											{
												// Labels didn't match. Reupload
												CANStackLogger::info("[TC]: Task controller localization labels do not match. DDOP will be deleted and reuploaded.");
												parentTC->set_state(StateMachineState::SendDeleteObjectPool);
											}
										}
									}
									else
									{
										CANStackLogger::warn("[TC]: Localization label message received, but ignored due to current state machine state.");
									}
								}
								break;

								case DeviceDescriptorCommands::RequestObjectPoolTransferResponse:
								{
									if (StateMachineState::WaitForRequestTransferObjectPoolResponse == parentTC->get_state())
									{
										if (0 == messageData[1])
										{
											// Because there is overhead associated with object storage, it is impossible to predict whether there is enough memory available, technically.
											CANStackLogger::debug("[TC]: Server indicates there may be enough memory available.");
											parentTC->set_state(StateMachineState::BeginTransferDDOP);
										}
										else
										{
											CANStackLogger::error("[TC]: Server states that there is not enough memory available for our DDOP. Client will terminate.");
											parentTC->terminate();
										}
									}
									else
									{
										CANStackLogger::warn("[TC]: Request Object-pool Transfer Response message received, but ignored due to current state machine state.");
									}
								}
								break;

								case DeviceDescriptorCommands::ObjectPoolActivateDeactivateResponse:
								{
									if (0 == messageData[1])
									{
										CANStackLogger::info("[TC]: DDOP Activated without error.");
										parentTC->set_state(StateMachineState::Connected);
									}
									else
									{
										CANStackLogger::error("[TC]: DDOP was not activated.");
										if (0x01 & messageData[1])
										{
											CANStackLogger::error("[TC]: There are errors in the DDOP. Faulting parent ID: " +
											                      isobus::to_string(static_cast<int>(static_cast<std::uint16_t>(messageData[2]) |
											                                                         static_cast<std::uint16_t>(messageData[3] << 8))) +
											                      " Faulting object: " +
											                      isobus::to_string(static_cast<int>(static_cast<std::uint16_t>(messageData[4]) |
											                                                         static_cast<std::uint16_t>(messageData[5] << 8))));
											if (0x01 & messageData[6])
											{
												CANStackLogger::error("[TC]: Method or attribute not supported by the TC");
											}
											if (0x02 & messageData[6])
											{
												// In theory, we check for this before upload, so this should be nearly impossible.
												CANStackLogger::error("[TC]: Unknown object reference (missing object)");
											}
											if (0x04 & messageData[6])
											{
												CANStackLogger::error("[TC]: Unknown error (Any other error)");
											}
											if (0x08 & messageData[6])
											{
												CANStackLogger::error("[TC]: Device descriptor object pool was deleted from volatile memory");
											}
											if (0xF0 & messageData[6])
											{
												CANStackLogger::warn("[TC]: The TC sent illegal errors in the reserved bits of the response.");
											}
										}
										if (0x02 & messageData[1])
										{
											CANStackLogger::error("[TC]: Task Controller ran out of memory during activation.");
										}
										if (0x04 & messageData[1])
										{
											CANStackLogger::error("[TC]: Task Controller indicates an unknown error occurred.");
										}
										if (0x08 & messageData[1])
										{
											CANStackLogger::error("[TC]: A different DDOP with the same structure label already exists in the TC.");
										}
										if (0xF0 & messageData[1])
										{
											CANStackLogger::warn("[TC]: The TC sent illegal errors in the reserved bits of the response.");
										}
									}
								}
								break;

								case DeviceDescriptorCommands::ObjectPoolDeleteResponse:
								{
									// Message content of this is unreliable, the standard is ambigious on what to even check.
									// Plus, if the delete failed, the recourse is the same, always proceed.
									if (StateMachineState::WaitForDeleteObjectPoolResponse == parentTC->get_state())
									{
										parentTC->set_state(StateMachineState::SendRequestTransferObjectPool);
									}
								}
								break;

								default:
								{
									CANStackLogger::warn("[TC]: Unsupported device descriptor command message received. Message will be dropped.");
								}
								break;
							}
						}
						break;

						case ProcessDataCommands::StatusMessage:
						{
							// Many values in the status message were undefined in version 2 and before, so the
							// standard explicitly tells us to ignore those attributes. The only things that really
							// matter are that we got the mesesage, and bytes 5, 6 and 7.
							parentTC->tcStatusBitfield = messageData[4];
							parentTC->sourceAddressOfCommandBeingExecuted = messageData[5];
							parentTC->commandBeingExecuted = messageData[6];
							parentTC->serverStatusMessageTimestamp_ms = SystemTiming::get_timestamp_ms();
							if (StateMachineState::WaitForServerStatusMessage == parentTC->currentState)
							{
								parentTC->set_state(StateMachineState::SendWorkingSetMaster);
							}
						}
						break;

						case ProcessDataCommands::ClientTask:
						{
							CANStackLogger::warn("[TC]: Server sent the client task message, which is not meant to be sent by servers.");
						}
						break;

						default:
						{
						}
						break;
					}
				}
				break;

				default:
				{
				}
				break;
			}
		}
	}

	bool TaskControllerClient::process_internal_object_pool_upload_callback(std::uint32_t callbackIndex,
	                                                                        std::uint32_t bytesOffset,
	                                                                        std::uint32_t numberOfBytesNeeded,
	                                                                        std::uint8_t *chunkBuffer,
	                                                                        void *parentPointer)
	{
		auto parentTCClient = static_cast<TaskControllerClient *>(parentPointer);
		bool retVal = false;

		// These assertions should never fail, but if they do, please consider reporting it on our GitHub page
		// along with a CAN trace and accompanying CANStackLogger output of the issue.
		assert(nullptr != parentTCClient);
		assert(nullptr != chunkBuffer);
		assert(0 != numberOfBytesNeeded);

		if ((bytesOffset + numberOfBytesNeeded) <= parentTCClient->binaryDDOP.size() + 1)
		{
			retVal = true;
			if (0 == bytesOffset)
			{
				chunkBuffer[0] = static_cast<std::uint8_t>(ProcessDataCommands::DeviceDescriptor) |
				  (static_cast<std::uint8_t>(DeviceDescriptorCommands::ObjectPoolTransfer) << 4);
				memcpy(&chunkBuffer[1], &parentTCClient->binaryDDOP[bytesOffset], numberOfBytesNeeded - 1);
			}
			else
			{
				// Subtract off 1 to account for the mux in the first byte of the message
				memcpy(chunkBuffer, &parentTCClient->binaryDDOP[bytesOffset - 1], numberOfBytesNeeded);
			}
		}
		else
		{
			CANStackLogger::error("[TC]: DDOP internal data callback received out of range request.");
		}
		return retVal;
	}

	void TaskControllerClient::process_tx_callback(std::uint32_t parameterGroupNumber,
	                                               std::uint32_t,
	                                               InternalControlFunction *,
	                                               ControlFunction *destinationControlFunction,
	                                               bool successful,
	                                               void *parentPointer)
	{
		if ((nullptr != parentPointer) &&
		    (static_cast<std::uint32_t>(CANLibParameterGroupNumber::ProcessData) == parameterGroupNumber) &&
		    (nullptr != destinationControlFunction))
		{
			TaskControllerClient *parent = reinterpret_cast<TaskControllerClient *>(parentPointer);

			if (StateMachineState::WaitForDDOPTransfer == parent->get_state())
			{
				if (successful)
				{
					parent->set_state(StateMachineState::SendObjectPoolActivate);
				}
				else
				{
					CANStackLogger::error("[TC]: DDOP upload did not complete. Resetting.");
					parent->set_state(StateMachineState::Disconnected);
				}
			}
		}
	}

	bool TaskControllerClient::send_delete_object_pool() const
	{
		return send_generic_process_data(static_cast<std::uint8_t>(ProcessDataCommands::DeviceDescriptor) |
		                                 (static_cast<std::uint8_t>(DeviceDescriptorCommands::ObjectPoolDelete) << 4));
	}

	bool TaskControllerClient::send_generic_process_data(std::uint8_t multiplexor) const
	{
		const std::array<std::uint8_t, CAN_DATA_LENGTH> buffer = { multiplexor,
			                                                         0xFF,
			                                                         0xFF,
			                                                         0xFF,
			                                                         0xFF,
			                                                         0xFF,
			                                                         0xFF,
			                                                         0xFF };

		return CANNetworkManager::CANNetwork.send_can_message(static_cast<std::uint32_t>(CANLibParameterGroupNumber::ProcessData),
		                                                      buffer.data(),
		                                                      CAN_DATA_LENGTH,
		                                                      myControlFunction.get(),
		                                                      partnerControlFunction.get());
	}

	bool TaskControllerClient::send_object_pool_activate() const
	{
		return send_generic_process_data(static_cast<std::uint8_t>(ProcessDataCommands::DeviceDescriptor) |
		                                 (static_cast<std::uint8_t>(DeviceDescriptorCommands::ObjectPoolActivateDeactivate) << 4));
	}

	bool TaskControllerClient::send_object_pool_deactivate() const
	{
		const std::array<std::uint8_t, CAN_DATA_LENGTH> buffer = { static_cast<std::uint8_t>(ProcessDataCommands::DeviceDescriptor) |
			                                                           (static_cast<std::uint8_t>(DeviceDescriptorCommands::ObjectPoolActivateDeactivate) << 4),
			                                                         0x00,
			                                                         0xFF,
			                                                         0xFF,
			                                                         0xFF,
			                                                         0xFF,
			                                                         0xFF,
			                                                         0xFF };

		return CANNetworkManager::CANNetwork.send_can_message(static_cast<std::uint32_t>(CANLibParameterGroupNumber::ProcessData),
		                                                      buffer.data(),
		                                                      CAN_DATA_LENGTH,
		                                                      myControlFunction.get(),
		                                                      partnerControlFunction.get());
	}

	bool TaskControllerClient::send_request_localization_label() const
	{
		return send_generic_process_data(static_cast<std::uint8_t>(ProcessDataCommands::DeviceDescriptor) |
		                                 (static_cast<std::uint8_t>(DeviceDescriptorCommands::RequestLocalizationLabel) << 4));
	}

	bool TaskControllerClient::send_request_object_pool_transfer() const
	{
		std::size_t binaryPoolSize = binaryDDOP.size();
		const std::array<std::uint8_t, CAN_DATA_LENGTH> buffer = { static_cast<std::uint8_t>(ProcessDataCommands::DeviceDescriptor) |
			                                                           (static_cast<std::uint8_t>(DeviceDescriptorCommands::RequestObjectPoolTransfer) << 4),
			                                                         static_cast<std::uint8_t>(binaryPoolSize & 0xFF),
			                                                         static_cast<std::uint8_t>((binaryPoolSize >> 8) & 0xFF),
			                                                         static_cast<std::uint8_t>((binaryPoolSize >> 16) & 0xFF),
			                                                         static_cast<std::uint8_t>((binaryPoolSize >> 24) & 0xFF),
			                                                         0xFF,
			                                                         0xFF,
			                                                         0xFF };

		return CANNetworkManager::CANNetwork.send_can_message(static_cast<std::uint32_t>(CANLibParameterGroupNumber::ProcessData),
		                                                      buffer.data(),
		                                                      CAN_DATA_LENGTH,
		                                                      myControlFunction.get(),
		                                                      partnerControlFunction.get());
	}

	bool TaskControllerClient::send_request_structure_label() const
	{
		// When all bytes are 0xFF, the TC will tell us about the latest structure label
		return send_generic_process_data(static_cast<std::uint8_t>(ProcessDataCommands::DeviceDescriptor) |
		                                 (static_cast<std::uint8_t>(DeviceDescriptorCommands::RequestStructureLabel) << 4));
	}

	bool TaskControllerClient::send_request_version_response() const
	{
		const std::array<std::uint8_t, CAN_DATA_LENGTH> buffer = { (static_cast<std::uint8_t>(TechnicalDataMessageCommands::ParameterVersion) << 4),
			                                                         static_cast<std::uint8_t>(Version::SecondPublishedEdition),
			                                                         0xFF, // Must be 0xFF when a client sends it (boot time)
			                                                         static_cast<std::uint8_t>(static_cast<std::uint8_t>(supportsDocumentation) |
			                                                                                   (static_cast<std::uint8_t>(supportsTCGEOWithoutPositionBasedControl) << 1) |
			                                                                                   (static_cast<std::uint8_t>(supportsTCGEOWithPositionBasedControl) << 2) |
			                                                                                   (static_cast<std::uint8_t>(supportsPeerControlAssignment) << 3) |
			                                                                                   (static_cast<std::uint8_t>(supportsImplementSectionControl) << 4)),
			                                                         0x00,
			                                                         numberBoomsSupported,
			                                                         numberSectionsSupported,
			                                                         numberChannelsSupportedForPositionBasedControl };

		return CANNetworkManager::CANNetwork.send_can_message(static_cast<std::uint32_t>(CANLibParameterGroupNumber::ProcessData),
		                                                      buffer.data(),
		                                                      CAN_DATA_LENGTH,
		                                                      myControlFunction.get(),
		                                                      partnerControlFunction.get());
	}

	bool TaskControllerClient::send_status() const
	{
		const std::array<std::uint8_t, CAN_DATA_LENGTH> buffer = { static_cast<std::uint8_t>(ProcessDataCommands::ClientTask) | 0xF0,
			                                                         0xFF, // Element number N/A
			                                                         0xFF, // DDI N/A
			                                                         0xFF, // DDI N/A
			                                                         static_cast<std::uint8_t>(tcStatusBitfield & 0x01), // Actual TC or DL status
			                                                         0x00, // Reserved (0)
			                                                         0x00, // Reserved (0)
			                                                         0x00 }; // Reserved (0)

		return CANNetworkManager::CANNetwork.send_can_message(static_cast<std::uint32_t>(CANLibParameterGroupNumber::ProcessData),
		                                                      buffer.data(),
		                                                      CAN_DATA_LENGTH,
		                                                      myControlFunction.get(),
		                                                      partnerControlFunction.get());
	}

	bool TaskControllerClient::send_version_request() const
	{
		return send_generic_process_data(static_cast<std::uint8_t>(TechnicalDataMessageCommands::ParameterRequestVersion));
	}

	bool TaskControllerClient::send_working_set_master() const
	{
		const std::array<std::uint8_t, CAN_DATA_LENGTH> buffer = { numberOfWorkingSetMembers, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

		return CANNetworkManager::CANNetwork.send_can_message(static_cast<std::uint32_t>(CANLibParameterGroupNumber::WorkingSetMaster),
		                                                      buffer.data(),
		                                                      CAN_DATA_LENGTH,
		                                                      myControlFunction.get(),
		                                                      nullptr);
	}

	void TaskControllerClient::set_state(StateMachineState newState)
	{
		if (newState != currentState)
		{
			stateMachineTimestamp_ms = SystemTiming::get_timestamp_ms();
			currentState = newState;
		}
	}

	void TaskControllerClient::worker_thread_function()
	{
		for (;;)
		{
			if (shouldTerminate)
			{
				break;
			}
			update();
			std::this_thread::sleep_for(std::chrono::milliseconds(50));
		}
	}

	TaskControllerClient::StateMachineState TaskControllerClient::get_state() const
	{
		return currentState;
	}

	std::uint8_t TaskControllerClient::get_connected_tc_number_booms_supported() const
	{
		return serverNumberOfBoomsForSectionControl;
	}

	std::uint8_t TaskControllerClient::get_connected_tc_number_sections_supported() const
	{
		return serverNumberOfSectionsForSectionControl;
	}

	std::uint8_t TaskControllerClient::get_connected_tc_number_channels_supported() const
	{
		return serverNumberOfChannelsForPositionBasedControl;
	}

	std::uint8_t TaskControllerClient::get_connected_tc_max_boot_time() const
	{
		return maxServerBootTime_s;
	}

	bool TaskControllerClient::get_connected_tc_option_supported(ServerOptions option) const
	{
		return (0 != (static_cast<std::uint8_t>(option) & serverOptionsByte1));
	}

	TaskControllerClient::Version TaskControllerClient::get_connected_tc_version() const
	{
		Version retVal = Version::Unknown;

		if (serverVersion <= static_cast<std::uint8_t>(Version::SecondPublishedEdition))
		{
			retVal = static_cast<Version>(serverVersion);
		}
		return retVal;
	}

} // namespace isobus