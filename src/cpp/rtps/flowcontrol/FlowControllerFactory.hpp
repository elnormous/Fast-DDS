#ifndef _RTPS_FLOWCONTROL_FLOWCONTROLLERFACTORY_HPP_
#define _RTPS_FLOWCONTROL_FLOWCONTROLLERFACTORY_HPP_

#include <fastdds/rtps/flowcontrol/FlowControllerDescriptor.hpp>
#include "FlowController.hpp"

#include <string>
#include <map>

namespace eprosima {
namespace fastdds {
namespace rtps {

class FlowControllerFactory
{
public:

    void init();

    /*!
     * Registers a new flow controller.
     * This function should be used by the participant.
     *
     * @param flow_controller_descr FlowController descriptor.
     */
    void register_flow_controller (
            const FlowControllerDescriptor& flow_controller_descr);

    /*!
     * Get a FlowController given its name.
     *
     * @param flow_controller_name Name of the interested FlowController.
     * @return Pointer to the FlowController. nullptr if no registered FlowController with that name.
     */
    FlowController* retrieve_flow_controller(
            const std::string& flow_controller_name);

private:

    //! Stores the created flow controllers.
    std::map<std::string, FlowController*> flow_controllers_;

};

} // namespace rtps
} // namespace fastdds
} // namespace eprosima

#endif // _RTPS_FLOWCONTROL_FLOWCONTROLLERFACTORY_HPP_
