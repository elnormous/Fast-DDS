#include "FlowControllerFactory.hpp"
#include "FlowControllerImpl.hpp"

namespace eprosima {
namespace fastdds {
namespace rtps {

constexpr char* const pure_sync_flow_congtroller_name = "PureSyncFlowController";
constexpr char* const sync_flow_congtroller_name = "SyncFlowController";

FlowControllerFactory::~FlowControllerFactory()
{
    std::for_each(flow_controllers_.begin(), flow_controllers_.end(),
            [](const std::pair<std::string, FlowController*>& flow_controller)
            {
                delete flow_controller.second;
            });
    flow_controllers_.clear();
}

void FlowControllerFactory::init()
{
    // Create default flow controllers.

    // PureSyncFlowController -> used by volatile besteffort writers.
    flow_controllers_.insert({pure_sync_flow_congtroller_name,
                              new FlowControllerImpl<FlowControllerPureSyncPublishMode, FlowControllerFifoSchedule>()});
    // SyncFlowController -> used by rest of besteffort writers.
    flow_controllers_.insert({sync_flow_congtroller_name,
                              new FlowControllerImpl<FlowControllerSyncPublishMode, FlowControllerFifoSchedule>()});
}

void FlowControllerFactory::register_flow_controller (
        const FlowControllerDescriptor& flow_controller_descr)
{
    // TODO
    // Register new flow controller.
}

/*!
 * Get a FlowController given its name.
 *
 * @param flow_controller_name Name of the interested FlowController.
 * @return Pointer to the FlowController. nullptr if no registered FlowController with that name.
 */
FlowController* FlowControllerFactory::retrieve_flow_controller(
        const std::string& flow_controller_name,
        const fastrtps::rtps::WriterAttributes& writer_attributes)
{
    FlowController* returned_flow = nullptr;

    // Detect it has to be returned a default flow_controller.
    if (0 == flow_controller_name.compare(FASTDDS_FLOW_CONTROLLER_DEFAULT))
    {
        if (fastrtps::rtps::SYNCHRONOUS_WRITER == writer_attributes.mode)
        {
            if (fastrtps::rtps::BEST_EFFORT == writer_attributes.endpoint.reliabilityKind &&
                    fastrtps::rtps::VOLATILE == writer_attributes.endpoint.durabilityKind)
            {
                returned_flow = flow_controllers_[pure_sync_flow_congtroller_name];
            }
            else
            {
                returned_flow = flow_controllers_[sync_flow_congtroller_name];
            }
        }
    }

    if (nullptr != returned_flow)
    {
        returned_flow->init();
    }

    return returned_flow;
}

} // namespace rtps
} // namespace fastdds
} // namespace eprosima
