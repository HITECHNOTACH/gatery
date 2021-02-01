#include "MemoryDetector.h"

#include "Circuit.h"

#include "coreNodes/Node_Signal.h"
#include "coreNodes/Node_Register.h"
#include "coreNodes/Node_Constant.h"
#include "coreNodes/Node_Compare.h"
#include "coreNodes/Node_Logic.h"
#include "coreNodes/Node_Multiplexer.h"
#include "supportNodes/Node_Memory.h"
#include "supportNodes/Node_MemPort.h"
#include "GraphExploration.h"

#include <sstream>
#include <vector>
#include <set>
#include <optional>

namespace hcl::core::hlim {


MemoryGroup::MemoryGroup() : NodeGroup(GroupType::SFU)
{
}

void MemoryGroup::formAround(Node_Memory *memory, Circuit &circuit)
{
    m_memory = memory;
    m_memory->moveToGroup(this);

    // Initial naive grabbing of everything that might be usefull
    for (auto &np : m_memory->getDirectlyDriven(0)) {
        auto *port = dynamic_cast<Node_MemPort*>(np.node);
        HCL_ASSERT(port->isWritePort() || port->isReadPort());
        // Check all write ports
        if (port->isWritePort()) {
            HCL_ASSERT_HINT(!port->isReadPort(), "For now I don't want to mix read and write ports");
            m_writePorts.push_back({.node=port});
            port->moveToGroup(this);
        }
        // Check all read ports
        if (port->isReadPort()) {
            m_readPorts.push_back({.node = port});
            ReadPort &rp = m_readPorts.back();
            port->moveToGroup(this);
            rp.dataOutput = {.node = port, .port = (size_t)Node_MemPort::Outputs::rdData};

            NodePort readPortEnable = port->getNonSignalDriver((unsigned)Node_MemPort::Inputs::enable);

            // Figure out if the data output is registered (== synchronous).
            std::vector<BaseNode*> dataRegisterComponents;
            for (auto nh : port->exploreOutput((size_t)Node_MemPort::Outputs::rdData)) {
                // Any branches in the signal path would mean the unregistered output is also used, preventing register fusion.
                if (nh.isBranchingForward()) break;

                if (nh.isNodeType<Node_Register>()) {
                    auto dataReg = (Node_Register *) nh.node();
                    // The register needs to be enabled by the same signal as the read port.
                    if (dataReg->getNonSignalDriver(Node_Register::Input::ENABLE) != readPortEnable)
                        break;
                    // The register can't have a reset (since it's essentially memory).
                    if (dataReg->getNonSignalDriver(Node_Register::Input::RESET_VALUE).node != nullptr)
                        break;
                    dataRegisterComponents.push_back(nh.node());
                    rp.syncReadDataReg = dataReg;
                    break;
                } else if (nh.isSignal()) {
                    dataRegisterComponents.push_back(nh.node());
                } else break;
            }

            if (rp.syncReadDataReg != nullptr) {
                // Move the entire signal path and the data register into the memory group
                for (auto opt : dataRegisterComponents)
                    opt->moveToGroup(this);
                rp.dataOutput = {.node = rp.syncReadDataReg, .port = 0};

                dataRegisterComponents.clear();

                // Figure out if the optional output register is active.
                for (auto nh : rp.syncReadDataReg->exploreOutput(0)) {
                    // Any branches in the signal path would mean the unregistered output is also used, preventing register fusion.
                    if (nh.isBranchingForward()) break;

                    if (nh.isNodeType<Node_Register>()) {
                        auto dataReg = (Node_Register *) nh.node();
                        // Optional output register must be with the same clock as the sync memory read access.
                        // TODO: Actually, apparently this is not the case for intel?
                        if (dataReg->getClocks()[0] != rp.syncReadDataReg->getClocks()[0])
                            break;
                        dataRegisterComponents.push_back(nh.node());
                        rp.outputReg = dataReg;
                        break;
                    } else if (nh.isSignal()) {
                        dataRegisterComponents.push_back(nh.node());
                    } else break;
                }

                if (rp.outputReg) {
                    // Move the entire signal path and the optional output data register into the memory group
                    for (auto opt : dataRegisterComponents)
                        opt->moveToGroup(this);
                    rp.dataOutput = {.node = rp.outputReg, .port = 0};
                }
            }
        }
    }

    // Verify writing is only happening with one clock:
    {
        Node_MemPort *firstWritePort = nullptr;
        for (auto &np : m_memory->getDirectlyDriven(0)) {
            auto *port = dynamic_cast<Node_MemPort*>(np.node);
            if (port->isWritePort()) {
                if (firstWritePort == nullptr)
                    firstWritePort = port;
                else {
                    std::stringstream issues;
                    issues << "All write ports to a memory must have the same clock!\n";
                    issues << "from:\n" << firstWritePort->getStackTrace() << "\n and from:\n" << port->getStackTrace();
                    HCL_DESIGNCHECK_HINT(firstWritePort->getClocks()[0] == port->getClocks()[0], issues.str());
                }
            }
        }
    }
}

void MemoryGroup::lazyCreateFixupNodeGroup()
{
    if (m_fixupNodeGroup == nullptr) {
        m_fixupNodeGroup = m_parent->addChildNodeGroup(GroupType::ENTITY);
        m_fixupNodeGroup->recordStackTrace();
        m_fixupNodeGroup->setName("Memory_Helper");
        m_fixupNodeGroup->setComment("Auto generated to handle various memory access issues such as read during write and read modify write hazards.");
        moveInto(m_fixupNodeGroup);
    }
}



void MemoryGroup::convertPortDependencyToLogic(Circuit &circuit)
{
    // If an async read happens after a write, it must
    // check if an address collision occured and if so directly forward the new value.
    for (auto &rp : m_readPorts) {
        // Collect a list of all potentially conflicting write ports and sort them in write order, so that conflict resolution can also happen in write order
        std::vector<WritePort*> sortedWritePorts;
        for (auto &wp : m_writePorts) 
            if (wp.node->isOrderedBefore(rp.node)) 
                sortedWritePorts.push_back(&wp);

        // sort last to first because multiplexers are prepended.
        // todo: this assumes that write ports do have an order.
        std::sort(sortedWritePorts.begin(), sortedWritePorts.end(), [](WritePort *left, WritePort *right)->bool{
            return left->node->isOrderedAfter(right->node);
        });

        for (auto wp_ptr : sortedWritePorts) {
            auto &wp = *wp_ptr;

            lazyCreateFixupNodeGroup();

            auto *addrCompNode = circuit.createNode<Node_Compare>(Node_Compare::EQ);
            addrCompNode->recordStackTrace();
            addrCompNode->moveToGroup(m_fixupNodeGroup);
            addrCompNode->setComment("Compare read and write addr for conflicts");
            addrCompNode->connectInput(0, rp.node->getDriver((unsigned)Node_MemPort::Inputs::address));
            addrCompNode->connectInput(1, wp.node->getDriver((unsigned)Node_MemPort::Inputs::address));

            NodePort conflict = {.node = addrCompNode, .port = 0ull};
            circuit.appendSignal(conflict)->setName("conflict");

            if (rp.node->getDriver((unsigned)Node_MemPort::Inputs::enable).node != nullptr) {
                auto *logicAnd = circuit.createNode<Node_Logic>(Node_Logic::AND);
                logicAnd->moveToGroup(m_fixupNodeGroup);
                logicAnd->recordStackTrace();
                logicAnd->connectInput(0, conflict);
                logicAnd->connectInput(1, rp.node->getDriver((unsigned)Node_MemPort::Inputs::enable));
                conflict = {.node = logicAnd, .port = 0ull};
                circuit.appendSignal(conflict)->setName("conflict");
            }

            HCL_ASSERT(wp.node->getNonSignalDriver((unsigned)Node_MemPort::Inputs::enable) == wp.node->getNonSignalDriver((unsigned)Node_MemPort::Inputs::wrEnable));
            if (wp.node->getDriver((unsigned)Node_MemPort::Inputs::enable).node != nullptr) {
                auto *logicAnd = circuit.createNode<Node_Logic>(Node_Logic::AND);
                logicAnd->moveToGroup(m_fixupNodeGroup);
                logicAnd->recordStackTrace();
                logicAnd->connectInput(0, conflict);
                logicAnd->connectInput(1, wp.node->getDriver((unsigned)Node_MemPort::Inputs::enable));
                conflict = {.node = logicAnd, .port = 0ull};
                circuit.appendSignal(conflict)->setName("conflict");
            }

            NodePort wrData = wp.node->getDriver((unsigned)Node_MemPort::Inputs::wrData);

            auto delayLike = [&](Node_Register *refReg, NodePort &np, const char *name, const char *comment) {
                auto *reg = circuit.createNode<Node_Register>();
                reg->recordStackTrace();
                reg->moveToGroup(m_fixupNodeGroup);
                reg->setComment(comment);
                reg->setClock(refReg->getClocks()[0]);
                for (auto i : {Node_Register::Input::ENABLE, Node_Register::Input::RESET_VALUE})
                    reg->connectInput(i, refReg->getDriver(i));
                reg->connectInput(Node_Register::Input::DATA, np);
                np = {.node = reg, .port = 0ull};
                circuit.appendSignal(np)->setName(name);
            };

            if (rp.syncReadDataReg != nullptr) {
                // read data gets delayed so we will have to delay the write data and conflict decision as well
                delayLike(rp.syncReadDataReg, wrData, "delayedWrData", "The memory read gets delayed by a register so the write data bypass also needs to be delayed.");
                delayLike(rp.syncReadDataReg, conflict, "delayedConflict", "The memory read gets delayed by a register so the collision detection decision also needs to be delayed.");

                if (rp.outputReg != nullptr) {
                    // need to delay even more
                    delayLike(rp.syncReadDataReg, wrData, "delayed_2_WrData", "The memory read gets delayed by an additional register so the write data bypass also needs to be delayed.");
                    delayLike(rp.syncReadDataReg, conflict, "delayed_2_Conflict", "The memory read gets delayed by an additional register so the collision detection decision also needs to be delayed.");
                }
            }

            std::vector<NodePort> consumers = rp.dataOutput.node->getDirectlyDriven(rp.dataOutput.port);

            // Finally the actual mux to arbitrate between the actual read and the forwarded write data.
            auto *muxNode = circuit.createNode<Node_Multiplexer>(2);

            // Then bind the mux
            muxNode->recordStackTrace();
            muxNode->moveToGroup(m_fixupNodeGroup);
            muxNode->setComment("If read and write addr match and read and write are enabled, forward write data to read output.");
            muxNode->connectSelector(conflict);
            muxNode->connectInput(0, rp.dataOutput);
            muxNode->connectInput(1, wrData);

            NodePort muxOut = {.node = muxNode, .port=0ull};

            circuit.appendSignal(muxOut)->setName("conflict_bypass_mux");

            // Rewire all original consumers to the mux output
            for (auto np : consumers)
                np.node->rewireInput(np.port, muxOut);
        }
    }

    // If two write ports have an explicit ordering, then the later write always trumps the former if both happen to the same address.
    // Search for such cases and build explicit logic that disables the earlier write.
    for (auto &wp1 : m_writePorts)
        for (auto &wp2 : m_writePorts) {
            if (&wp1 == &wp2) continue;

            if (wp1.node->isOrderedBefore(wp2.node)) {
                // Potential addr conflict, build hazard logic

                lazyCreateFixupNodeGroup();


                auto *addrCompNode = circuit.createNode<Node_Compare>(Node_Compare::NEQ);
                addrCompNode->recordStackTrace();
                addrCompNode->moveToGroup(m_fixupNodeGroup);
                addrCompNode->setComment("We can enable the former write if the write adresses differ.");
                addrCompNode->connectInput(0, wp1.node->getDriver((unsigned)Node_MemPort::Inputs::address));
                addrCompNode->connectInput(1, wp2.node->getDriver((unsigned)Node_MemPort::Inputs::address));

                // Enable write if addresses differ
                NodePort newWrEn1 = {.node = addrCompNode, .port = 0ull}; 
                circuit.appendSignal(newWrEn1)->setName("newWrEn");

                // Alternatively, enable write if wp2 does not write (no connection on enable means yes)
                HCL_ASSERT(wp2.node->getNonSignalDriver((unsigned)Node_MemPort::Inputs::enable) == wp2.node->getNonSignalDriver((unsigned)Node_MemPort::Inputs::wrEnable));
                if (wp2.node->getDriver((unsigned)Node_MemPort::Inputs::enable).node != nullptr) {

                    auto *logicNot = circuit.createNode<Node_Logic>(Node_Logic::NOT);
                    logicNot->moveToGroup(m_fixupNodeGroup);
                    logicNot->recordStackTrace();
                    logicNot->connectInput(0, wp2.node->getDriver((unsigned)Node_MemPort::Inputs::enable));

                    auto *logicOr = circuit.createNode<Node_Logic>(Node_Logic::OR);
                    logicOr->moveToGroup(m_fixupNodeGroup);
                    logicOr->setComment("We can also enable the former write if the latter write is disabled.");
                    logicOr->recordStackTrace();
                    logicOr->connectInput(0, newWrEn1);
                    logicOr->connectInput(1, {.node = logicNot, .port = 0ull});
                    newWrEn1 = {.node = logicOr, .port = 0ull};
                    circuit.appendSignal(newWrEn1)->setName("newWrEn");
                }

                // But only enable write if wp1 actually wants to write (no connection on enable means yes)
                HCL_ASSERT(wp1.node->getNonSignalDriver((unsigned)Node_MemPort::Inputs::enable) == wp1.node->getNonSignalDriver((unsigned)Node_MemPort::Inputs::wrEnable));
                if (wp1.node->getDriver((unsigned)Node_MemPort::Inputs::enable).node != nullptr) {
                    auto *logicAnd = circuit.createNode<Node_Logic>(Node_Logic::AND);
                    logicAnd->moveToGroup(m_fixupNodeGroup);
                    logicAnd->setComment("But we can only enable the former write if the former write actually wants to write.");
                    logicAnd->recordStackTrace();
                    logicAnd->connectInput(0, newWrEn1);
                    logicAnd->connectInput(1, wp1.node->getDriver((unsigned)Node_MemPort::Inputs::enable));
                    newWrEn1 = {.node = logicAnd, .port = 0ull};
                    circuit.appendSignal(newWrEn1)->setName("newWrEn");
                }


                wp1.node->rewireInput((unsigned)Node_MemPort::Inputs::enable, newWrEn1);
                wp1.node->rewireInput((unsigned)Node_MemPort::Inputs::wrEnable, newWrEn1);
            }
        }


    // Reorder all writes to happen after all reads
    Node_MemPort *lastPort = nullptr;
    for (auto &rp : m_readPorts) {
        rp.node->orderAfter(lastPort);
        lastPort = rp.node;
    }
    // Writes can happen in any order now, but after the last read
    for (auto &wp : m_writePorts)
        wp.node->orderAfter(lastPort);
}

void MemoryGroup::attemptRegisterRetiming(Circuit &circuit)
{
    if (m_memory->type() != Node_Memory::MemType::BRAM) return;

    // If we are aiming for blockrams:
    // Check if any read ports are lacking the register that make them synchronous.
    // If they do, scan the read data output bus for any registers buried in the combinatorics that could be pulled back and fused.
    // While doing that, also check for and build read modify write mechanics w. hazard detection in case those combinatorics feed back into
    // a write port of the same memory with same addr and enables.
    for (auto &rp : m_readPorts) {
        // It's fine if it is already synchronous.
        if (rp.syncReadDataReg != nullptr) continue;
        HCL_ASSERT(rp.outputReg == nullptr);

        // Walk through the graph looking for a write port, backtrack on any non-combinatorial nodes.
        // While we do so, keep track of any registers. Those would be merged backwards to make the read synchronous.
        // If we encounter any issues that would prevent BRAM formation, compile a list of those issues.
        // Also keep track of all nodes that would be delayed to later insert registers on their inputs from external networks.
        Node_MemPort *writePort = nullptr;
        std::vector<Node_Register*> registers;
        std::set<BaseNode*> delayedNodes;
        std::stringstream issues;
        issues << "Can't turn memory into blockram because an asynchronous read can not be turned into a synchronous one:\n";
        for (auto nh : rp.node->exploreOutput((unsigned)Node_MemPort::Outputs::rdData).skipDependencies()) {
            if (auto *port = dynamic_cast<Node_MemPort*>(nh.node())) {
                if (port->isReadPort()) {
                    HCL_ASSERT(!port->isWritePort());
                    // We encountered a read port.
                    // If this is a ROM, we are fine. Otherwise we have a problem because we can not delay the read by one tick.
                    if (!port->getMemory()->isROM()) {
                        issues 
                            << "Async read port feeds into a non-read-only memory, can't insert register to make synchronous without breaking read-write-timings on second memory.\n"
                            << "Read port from:\n";
                        issues << rp.node->getStackTrace();
                        issues 
                            << "Second non-read-only memory access:\n";
                        issues << port->getStackTrace();
                        HCL_DESIGNCHECK_HINT(false, issues.str());
                    }
                }
                if (port->isWritePort()) {
                    // RMW hazard fixing if writing to the same memory
                    if (port->getMemory() != m_memory) {
                        // This is also a problem
                        issues 
                            << "Async read port feeds into a write port of memory, can't insert register to make synchronous without breaking read-write-timings on second memory.\n"
                            << "Read port from:\n";
                        issues << rp.node->getStackTrace();
                        issues 
                            << "Second write memory access:\n";
                        issues << port->getStackTrace();
                        HCL_DESIGNCHECK_HINT(false, issues.str());
                    } else {
                        if (writePort == nullptr) {
                            writePort = port;
                            nh.backtrack();
                            continue;
                        } else {
                            HCL_ASSERT_HINT(false, "Duplicate write ports!");
                        }
                    }
                }
            } else
            if (auto *reg = dynamic_cast<Node_Register*>(nh.node())) {
                if (reg->getNonSignalDriver((unsigned)Node_Register::Input::RESET_VALUE).node != nullptr) {
                    issues 
                        << "Async read port feeds through combinatory nodes into a register with a reset value. The reset can value would change upon moving the register backwards.\n"
                        << "Read port from:\n";
                    issues << rp.node->getStackTrace();
                    issues 
                        << "Register:\n";
                    issues << reg->getStackTrace();
                    HCL_DESIGNCHECK_HINT(false, issues.str());
                }
                // Check clock and enable later
                registers.push_back(reg);
                nh.backtrack();
                continue;
            } else
            if (!nh.node()->isCombinatorial() || nh.node()->hasSideEffects()) {
                issues 
                    << "Async read port feeds into a non-combinatorial node or a node with side effects. Can't insert register.\n";
                issues 
                    << "Read port from:\n";
                issues << rp.node->getStackTrace();
                issues 
                    << "Offending node from:\n";
                issues << nh.node()->getStackTrace();
                HCL_DESIGNCHECK_HINT(false, issues.str());
            } else
                delayedNodes.insert(nh.node());
        }

        Clock *clock = nullptr;
        boost::optional<NodePort> enable;
        // Ensure everything is on the same clock:
        {
            std::stringstream issue;
            issue << "Can't turn memory into blockram because an asynchronous read can not be turned into a synchronous one: Following registers are using differing clocks.\n";
            if (writePort != nullptr) {
                clock = writePort->getClocks()[0];
                issue << "From:\n" << writePort->getStackTrace();
            }

            for (auto reg : registers) 
                if (clock == nullptr) {
                    clock = reg->getClocks()[0];
                    issue << "From:\n" << reg->getStackTrace();
                } else if (clock != reg->getClocks()[0]) {
                    issue << "and from:\n" << reg->getStackTrace();
                    HCL_DESIGNCHECK_HINT(false, issue.str());
                }
        }
        // Ensure everything is on the same enable:
        {
            std::stringstream issue;
            issue << "Can't turn memory into blockram because an asynchronous read can not be turned into a synchronous one: Following registers are using differing enables.\n";
            for (auto reg : registers) 
                if (!enable) {
                    enable = reg->getNonSignalDriver((unsigned)Node_Register::Input::ENABLE);
                    issue << "From:\n" << reg->getStackTrace();
                } else if (*enable != reg->getNonSignalDriver((unsigned)Node_Register::Input::ENABLE)) {
                    issue << "and from:\n" << reg->getStackTrace();
                    HCL_DESIGNCHECK_HINT(false, issue.str());
                }
        }

        // Get to work
        if (!enable)
            enable = NodePort{};

        auto insertDelayOutput = [&](NodePort &np, NodeGroup *ng, const char *comment)->Node_Register* {
            std::vector<NodePort> consumers = np.node->getDirectlyDriven(np.port);

            auto *reg = circuit.createNode<Node_Register>();
            reg->recordStackTrace();
            reg->moveToGroup(ng);
            reg->setComment(comment);
            reg->setClock(clock);
            
            reg->connectInput(Node_Register::Input::ENABLE, *enable);
            reg->connectInput(Node_Register::Input::DATA, np);
            np = {.node = reg, .port = 0ull};
            for (auto c : consumers)
                c.node->rewireInput(c.port, np);
            return reg;
        };
        auto insertDelayInput = [&](NodePort np, NodeGroup *ng, const char *comment)->Node_Register* {
            auto driver = np.node->getDriver(np.port);
            auto *reg = circuit.createNode<Node_Register>();
            reg->recordStackTrace();
            reg->moveToGroup(ng);
            reg->setComment(comment);
            reg->setClock(clock);
            
            reg->connectInput(Node_Register::Input::ENABLE, *enable);
            reg->connectInput(Node_Register::Input::DATA, driver);
            np.node->rewireInput(np.port, {.node=reg, .port=0ull});
            return reg;
        };

        // insert delay into read bus:
        rp.syncReadDataReg = insertDelayOutput(rp.dataOutput, this, "");

        // bypass output registers
        for (auto reg : registers) 
            reg->bypassOutputToInput(0, (unsigned)Node_Register::Input::DATA);

        // insert delays on other inputs
        for (auto n : delayedNodes) {
            for (auto i : utils::Range(n->getNumInputPorts())) {
                auto driver = n->getDriver(i);
                if (driver.node == nullptr) continue;
                if (driver.node->getOutputConnectionType(driver.port).interpretation == ConnectionType::DEPENDENCY) continue; // TODO: think about this
                if (!delayedNodes.contains(driver.node) && driver.node != rp.syncReadDataReg) {
                    insertDelayInput({.node=n, .port=i}, n->getGroup(), "Auto generated register");
                }                
            }
        }


        if (writePort != nullptr) {
            lazyCreateFixupNodeGroup();

            auto *delayedWrData = circuit.createNode<Node_Register>();
            delayedWrData->recordStackTrace();
            delayedWrData->moveToGroup(m_fixupNodeGroup);
            delayedWrData->setClock(clock);
            
            delayedWrData->connectInput(Node_Register::Input::ENABLE, *enable);
            delayedWrData->connectInput(Node_Register::Input::DATA, writePort->getDriver((unsigned)Node_MemPort::Inputs::wrData));

            insertDelayInput({.node=writePort, .port=(unsigned)Node_MemPort::Inputs::address}, m_fixupNodeGroup, "");

            HCL_ASSERT(writePort->getNonSignalDriver((unsigned)Node_MemPort::Inputs::enable) == writePort->getNonSignalDriver((unsigned)Node_MemPort::Inputs::wrEnable));
            insertDelayInput({.node=writePort, .port=(unsigned)Node_MemPort::Inputs::enable}, m_fixupNodeGroup, "");
            writePort->rewireInput((unsigned)Node_MemPort::Inputs::wrEnable, writePort->getDriver((unsigned)Node_MemPort::Inputs::enable));

            auto *addrCompNode = circuit.createNode<Node_Compare>(Node_Compare::EQ);
            addrCompNode->recordStackTrace();
            addrCompNode->moveToGroup(m_fixupNodeGroup);
            addrCompNode->setComment("Compare read and write addr for conflicts");
            addrCompNode->connectInput(0, rp.node->getDriver((unsigned)Node_MemPort::Inputs::address));
            addrCompNode->connectInput(1, writePort->getDriver((unsigned)Node_MemPort::Inputs::address));

            NodePort conflict = {.node = addrCompNode, .port = 0ull};

            if (rp.node->getDriver((unsigned)Node_MemPort::Inputs::enable).node != nullptr) {
                auto *logicAnd = circuit.createNode<Node_Logic>(Node_Logic::AND);
                logicAnd->moveToGroup(m_fixupNodeGroup);
                logicAnd->recordStackTrace();
                logicAnd->connectInput(0, conflict);
                logicAnd->connectInput(1, rp.node->getDriver((unsigned)Node_MemPort::Inputs::enable));
                conflict = {.node = logicAnd, .port = 0ull};
            }

            if (writePort->getDriver((unsigned)Node_MemPort::Inputs::enable).node != nullptr) {
                auto *logicAnd = circuit.createNode<Node_Logic>(Node_Logic::AND);
                logicAnd->moveToGroup(m_fixupNodeGroup);
                logicAnd->recordStackTrace();
                logicAnd->connectInput(0, conflict);
                logicAnd->connectInput(1, writePort->getDriver((unsigned)Node_MemPort::Inputs::enable));
                conflict = {.node = logicAnd, .port = 0ull};
            }

            std::vector<NodePort> consumers = rp.dataOutput.node->getDirectlyDriven(rp.dataOutput.port);

            // Finally the actual mux to arbitrate between the actual read and the forwarded write data.
            auto *muxNode = circuit.createNode<Node_Multiplexer>(2);

            // Then bind the mux
            muxNode->recordStackTrace();
            muxNode->moveToGroup(m_fixupNodeGroup);
            muxNode->setComment("If read and write addr match and read and write are enabled, forward write data to read output.");
            muxNode->connectSelector(conflict);
            muxNode->connectInput(0, rp.dataOutput);
            muxNode->connectInput(1, {.node=delayedWrData, .port=0ull});

            // Rewire all original consumers to the mux output
            for (auto np : consumers)
                np.node->rewireInput(np.port, {.node=muxNode, .port=0ull});
        }       
    }
}

void MemoryGroup::verify()
{
    switch (m_memory->type()) {
        case Node_Memory::MemType::BRAM: 
            for (auto &rp : m_readPorts) {
                std::stringstream issue;
                issue << "Memory can not become BRAM because a read port is missing it's data register.\nMemory from:\n" 
                      << m_memory->getStackTrace() << "\nRead port from:\n" << rp.node->getStackTrace();
                HCL_DESIGNCHECK_HINT(rp.syncReadDataReg != nullptr, issue.str());
            }
            if (m_readPorts.size() + m_writePorts.size() > 2) {
                std::stringstream issue;
                issue << "Memory can not become BRAM because it has too many memory ports.\nMemory from:\n" 
                      << m_memory->getStackTrace();
                HCL_DESIGNCHECK_HINT(false, issue.str());
            }
        break;
        case Node_Memory::MemType::LUTRAM:
            if (m_readPorts.size() > 1) {
                std::stringstream issue;
                issue << "Memory can not become LUTRAM because it has too many read ports.\nMemory from:\n" 
                      << m_memory->getStackTrace();
                HCL_DESIGNCHECK_HINT(false, issue.str());
            }
            if (m_writePorts.size() > 1) {
                std::stringstream issue;
                issue << "Memory can not become LUTRAM because it has too many write ports.\nMemory from:\n" 
                      << m_memory->getStackTrace();
                HCL_DESIGNCHECK_HINT(false, issue.str());
            }
        break;
    }
}



void findMemoryGroups(Circuit &circuit)
{
    for (auto &node : circuit.getNodes())
        if (auto *memory = dynamic_cast<Node_Memory*>(node.get())) {
            auto *memoryGroup = memory->getGroup()->addSpecialChildNodeGroup<MemoryGroup>();
            memoryGroup->setName("memory");
            memoryGroup->setComment("Auto generated");
            memoryGroup->formAround(memory, circuit);
        }
}

void buildExplicitMemoryCircuitry(Circuit &circuit)
{
    for (auto &node : circuit.getNodes())
        if (auto *memory = dynamic_cast<Node_Memory*>(node.get())) {
            auto *memoryGroup = dynamic_cast<MemoryGroup*>(memory->getGroup());
            memoryGroup->convertPortDependencyToLogic(circuit);
            memoryGroup->attemptRegisterRetiming(circuit);
            memoryGroup->verify();
        }
}


}