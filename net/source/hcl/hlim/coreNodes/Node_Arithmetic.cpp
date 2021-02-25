#include "Node_Arithmetic.h"

#include "../../utils/BitManipulation.h"

namespace hcl::core::hlim {

Node_Arithmetic::Node_Arithmetic(Op op) : Node(2, 1), m_op(op)
{

}

void Node_Arithmetic::connectInput(size_t operand, const NodePort &port)
{
    NodeIO::connectInput(operand, port);
    updateConnectionType();
}

void Node_Arithmetic::updateConnectionType()
{
    auto lhs = getDriver(0);
    auto rhs = getDriver(1);

    ConnectionType desiredConnectionType = getOutputConnectionType(0);

    if (lhs.node != nullptr) {
        desiredConnectionType = hlim::getOutputConnectionType(lhs);
        if (rhs.node != nullptr) {
            desiredConnectionType.width = std::max(desiredConnectionType.width, getOutputWidth(rhs));
            HCL_ASSERT_HINT(lhs.node->getOutputConnectionType(lhs.port).interpretation == rhs.node->getOutputConnectionType(rhs.port).interpretation, "Mixing different interpretations not yet implemented!");
        }
    } else if (rhs.node != nullptr)
        desiredConnectionType = hlim::getOutputConnectionType(rhs);

    setOutputConnectionType(0, desiredConnectionType);
}


void Node_Arithmetic::disconnectInput(size_t operand)
{
    NodeIO::disconnectInput(operand);
}


void Node_Arithmetic::simulateEvaluate(sim::SimulatorCallbacks &simCallbacks, sim::DefaultBitVectorState &state, const size_t *internalOffsets, const size_t *inputOffsets, const size_t *outputOffsets) const
{
    HCL_ASSERT_HINT(getOutputConnectionType(0).width <= 64, "Arithmetic with more than 64 bits not yet implemented!");
    auto leftDriver = getNonSignalDriver(0);
    auto rightDriver = getNonSignalDriver(1);
    if (leftDriver.node == nullptr || rightDriver.node == nullptr) {
        state.setRange(sim::DefaultConfig::DEFINED, outputOffsets[0], getOutputConnectionType(0).width, false);
        return;
    }

    const auto &leftType = leftDriver.node->getOutputConnectionType(leftDriver.port);
    const auto &rightType = rightDriver.node->getOutputConnectionType(rightDriver.port);
    HCL_ASSERT_HINT(leftType.width <= 64, "Arithmetic with more than 64 bits not yet implemented!");
    HCL_ASSERT_HINT(rightType.width <= 64, "Arithmetic with more than 64 bits not yet implemented!");

    if (!allDefinedNonStraddling(state, inputOffsets[0], leftType.width)) {
        state.setRange(sim::DefaultConfig::DEFINED, outputOffsets[0], getOutputConnectionType(0).width, false);
        return;
    }

    if (!allDefinedNonStraddling(state, inputOffsets[1], rightType.width)) {
        state.setRange(sim::DefaultConfig::DEFINED, outputOffsets[0], getOutputConnectionType(0).width, false);
        return;
    }

    std::uint64_t left = state.extractNonStraddling(sim::DefaultConfig::VALUE, inputOffsets[0], leftType.width);
    std::uint64_t right = state.extractNonStraddling(sim::DefaultConfig::VALUE, inputOffsets[1], rightType.width);
    std::uint64_t result;

    switch (getOutputConnectionType(0).interpretation) {
        case ConnectionType::BOOL:
            HCL_ASSERT_HINT(false, "Can't do arithmetic on booleans!");
        break;
        case ConnectionType::BITVEC:
            switch (m_op) {
                case ADD:
                    result = left + right;
                break;
                case SUB:
                    result = left - right;
                break;
                case MUL:
                    result = left * right;
                break;
                case DIV:
                    result = left / right;
                break;
                case REM:
                    result = left % right;
                break;
                default:
                    HCL_ASSERT_HINT(false, "Unhandled case!");
            }
        break;
        default:
            HCL_ASSERT_HINT(false, "Unhandled case!");
    }

    state.insertNonStraddling(sim::DefaultConfig::VALUE, outputOffsets[0], getOutputConnectionType(0).width, result);
    state.insertNonStraddling(sim::DefaultConfig::DEFINED, outputOffsets[0], getOutputConnectionType(0).width, ~0ull);
}


std::string Node_Arithmetic::getTypeName() const
{
    switch (m_op) {
        case ADD: return "add";
        case SUB: return "sub";
        case MUL: return "mul";
        case DIV: return "div";
        case REM: return "remainder";
        default: return "Arithmetic";
    }
}

void Node_Arithmetic::assertValidity() const
{

}

std::string Node_Arithmetic::getInputName(size_t idx) const
{
    return idx==0?"a":"b";
}

std::string Node_Arithmetic::getOutputName(size_t idx) const
{
    return "out";
}

std::unique_ptr<BaseNode> Node_Arithmetic::cloneUnconnected() const
{
    std::unique_ptr<BaseNode> res(new Node_Arithmetic(m_op));
    copyBaseToClone(res.get());
    return res;
}



}
