#include "FlowControllerFactory.hpp"

namespace eprosima {
namespace fastdds {
namespace rtps {

void FlowControllerFactory::init()
{
    // TODO
    // Create default flow controllers.
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
        const std::string& flow_controller_name)
{
    // TODO
    // Return the flow controller.
    return nullptr;
}

} // namespace rtps
} // namespace fastdds
} // namespace eprosima
