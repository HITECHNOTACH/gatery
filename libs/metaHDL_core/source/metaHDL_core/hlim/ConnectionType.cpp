#include "ConnectionType.h"

namespace mhdl {
namespace core {    
namespace hlim {


bool ConnectionType::operator==(const ConnectionType &rhs) const
{
    if (rhs.interpretation != interpretation) return false;
    if (rhs.width != width) return false;
    if (rhs.fixedPoint_denominator != fixedPoint_denominator) return false;
    if (rhs.float_signBit != float_signBit) return false;
    if (rhs.float_mantissaBits != float_mantissaBits) return false;
    if (rhs.float_exponentBias != float_exponentBias) return false;

    return true;
}
    
#if 0

unsigned CompoundConnectionType::getTotalWidth() const 
{
    unsigned sum = 0;
    for (const auto &sub : m_subConnections)
        sum += sub.type->getTotalWidth();
    return sum;
}

#endif

}
}
}

