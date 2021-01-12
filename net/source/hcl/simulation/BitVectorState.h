#pragma once

#include "../utils/Exceptions.h"
#include "../utils/Preprocessor.h"
#include "../utils/BitManipulation.h"
#include "../utils/Range.h"

#include <vector>
#include <array>
#include <cstdint>
#include <string.h>

namespace hcl::core::sim {

struct DefaultConfig
{
    using BaseType = std::size_t;
    enum {
        NUM_BITS_PER_BLOCK = sizeof(BaseType)*8
    };
    enum Plane {
        VALUE,
        DEFINED,
        NUM_PLANES
    };
};

template<class Config>
class BitVectorState
{
    public:
        void resize(size_t size);
        inline size_t size() const { return m_size; }
        inline size_t getNumBlocks() const { return m_values[0].size(); }
        void clear();
        
        bool get(typename Config::Plane plane, size_t idx) const;
        void set(typename Config::Plane plane, size_t idx);
        void set(typename Config::Plane plane, size_t idx, bool bit);
        void clear(typename Config::Plane plane, size_t idx);
        void toggle(typename Config::Plane plane, size_t idx);

        void setRange(typename Config::Plane plane, size_t offset, size_t size, bool bit);
        void setRange(typename Config::Plane plane, size_t offset, size_t size);
        void clearRange(typename Config::Plane plane, size_t offset, size_t size);
        void copyRange(size_t dstOffset, const BitVectorState<Config> &src, size_t srcOffset, size_t size);
        
        typename Config::BaseType *data(typename Config::Plane plane);
        const typename Config::BaseType *data(typename Config::Plane plane) const;
        
        BitVectorState<Config> extract(size_t start, size_t size) const;
        void insert(const BitVectorState& state, size_t offset);
        
        typename Config::BaseType extract(typename Config::Plane plane, size_t offset, size_t size) const;
        typename Config::BaseType extractNonStraddling(typename Config::Plane plane, size_t start, size_t size) const;

        void insert(typename Config::Plane plane, size_t start, size_t size, typename Config::BaseType value);
        void insertNonStraddling(typename Config::Plane plane, size_t start, size_t size, typename Config::BaseType value);
    protected:
        size_t m_size = 0;
        std::array<std::vector<typename Config::BaseType>, Config::NUM_PLANES> m_values;
};


template<typename Config>
bool allDefinedNonStraddling(const BitVectorState<Config> &vec, size_t start, size_t size) {
    return !utils::andNot(vec.extractNonStraddling(Config::DEFINED, start, size), utils::bitMaskRange(0, size));
}

using DefaultBitVectorState = BitVectorState<DefaultConfig>;

template<typename Config>
std::ostream& operator << (std::ostream& s, const BitVectorState<Config>& state)
{
    for (int i = state.size()-1; i >= 0; --i)
    {
        if (!state.get(Config::DEFINED, i))
            s << 'x';
        else if (state.get(Config::VALUE, i))
            s << '1';
        else
            s << '0';
    }
    return s;
}






template<class Config>
void BitVectorState<Config>::resize(size_t size)
{
    m_size = size;
    for (auto i : utils::Range<size_t>(Config::NUM_PLANES))
        m_values[i].resize((size+Config::NUM_BITS_PER_BLOCK-1) / Config::NUM_BITS_PER_BLOCK);
}

template<class Config>
void BitVectorState<Config>::clear()
{
    for (auto i : utils::Range<size_t>(Config::NUM_PLANES))
        m_values[i].clear();
}

template<class Config>
bool BitVectorState<Config>::get(typename Config::Plane plane, size_t idx) const
{
    return utils::bitExtract(m_values[plane].data(), idx);
}

template<class Config>
void BitVectorState<Config>::set(typename Config::Plane plane, size_t idx)
{
    utils::bitSet(m_values[plane].data(), idx);
}

template<class Config>
void BitVectorState<Config>::set(typename Config::Plane plane, size_t idx, bool bit)
{
    if (bit)
        utils::bitSet(m_values[plane].data(), idx);
    else
        utils::bitClear(m_values[plane].data(), idx);
}

template<class Config>
void BitVectorState<Config>::clear(typename Config::Plane plane, size_t idx)
{
    utils::bitClear(m_values[plane].data(), idx);
}

template<class Config>
void BitVectorState<Config>::toggle(typename Config::Plane plane, size_t idx)
{
    utils::bitToggle(m_values[plane].data(), idx);
}


template<class Config>
void BitVectorState<Config>::setRange(typename Config::Plane plane, size_t offset, size_t size, bool bit)
{
    typename Config::BaseType content = 0;
    if (bit)
        content = ~content;
    
    size_t firstWordSize;
    size_t wordOffset = offset / Config::NUM_BITS_PER_BLOCK;
    if (offset % Config::NUM_BITS_PER_BLOCK == 0) {
        firstWordSize = 0;
    } else {
        firstWordSize = std::min(size, Config::NUM_BITS_PER_BLOCK - offset % Config::NUM_BITS_PER_BLOCK);
        insertNonStraddling(plane, offset, firstWordSize, content);
        wordOffset++;
    }
    
    size_t numFullWords = (size - firstWordSize) / Config::NUM_BITS_PER_BLOCK;
    for (auto i : utils::Range(numFullWords))
        m_values[plane][wordOffset + i] = content;


    size_t trailingWordSize = (size - firstWordSize) % Config::NUM_BITS_PER_BLOCK;
    if (trailingWordSize > 0)
        insertNonStraddling(plane, offset + firstWordSize + numFullWords*Config::NUM_BITS_PER_BLOCK, trailingWordSize, content);
}

template<class Config>
void BitVectorState<Config>::setRange(typename Config::Plane plane, size_t offset, size_t size)
{
    setRange(plane, offset, size, true);
}

template<class Config>
void BitVectorState<Config>::clearRange(typename Config::Plane plane, size_t offset, size_t size)
{
    setRange(plane, offset, size, false);
}

template<class Config>
void BitVectorState<Config>::copyRange(size_t dstOffset, const BitVectorState<Config> &src, size_t srcOffset, size_t size)
{    
    ///@todo: Optimize aligned cases (which happen quite frequently!)
    size_t width = size;
    size_t offset = 0;
    while (offset < width) {
        size_t chunkSize = std::min<size_t>(Config::NUM_BITS_PER_BLOCK, width-offset);
        
        for (auto i : utils::Range<size_t>(Config::NUM_PLANES))
            insert((typename Config::Plane) i, dstOffset + offset, chunkSize,
                    src.extract((typename Config::Plane) i, srcOffset + offset, chunkSize));

        offset += chunkSize;
    }
}


template<class Config>
typename Config::BaseType *BitVectorState<Config>::data(typename Config::Plane plane)
{
    return m_values[plane].data();
}

template<class Config>
const typename Config::BaseType *BitVectorState<Config>::data(typename Config::Plane plane) const
{
    return m_values[plane].data();
}

template<class Config>
BitVectorState<Config> BitVectorState<Config>::extract(size_t start, size_t size) const
{
    BitVectorState<Config> result;
    result.resize(size);
    if (start % 8 == 0) {
        for (auto i : utils::Range<size_t>(Config::NUM_PLANES))
            memcpy((char*) result.data((typename Config::Plane) i), (char*) data((typename Config::Plane) i) + start/8, (size+7)/8);
    } else
        result.copyRange(0, *this, start, size);

    return result;
}

template<class Config>
inline void BitVectorState<Config>::insert(const BitVectorState& state, size_t offset)
{
    const size_t width = state.size();
    size_t srcOffset = 0;

    while (srcOffset < width) {
        size_t chunkSize = std::min<size_t>(64, width - srcOffset);

        auto val = state.extractNonStraddling(sim::DefaultConfig::VALUE, srcOffset, chunkSize);
        insertNonStraddling(sim::DefaultConfig::VALUE, offset, chunkSize, val);

        auto def = state.extractNonStraddling(sim::DefaultConfig::DEFINED, srcOffset, chunkSize);
        insertNonStraddling(sim::DefaultConfig::DEFINED, offset, chunkSize, def);

        offset += chunkSize;
        srcOffset += chunkSize;
    }
}

template<class Config>
inline typename Config::BaseType BitVectorState<Config>::extract(typename Config::Plane plane, size_t offset, size_t size) const
{
    HCL_ASSERT(size <= Config::NUM_BITS_PER_BLOCK);
    const auto* values = &m_values[plane][offset / Config::NUM_BITS_PER_BLOCK];
    const size_t wordOffset = offset % Config::NUM_BITS_PER_BLOCK;

    auto val = values[0];
    val >>= wordOffset;
    if(wordOffset + size > Config::NUM_BITS_PER_BLOCK)
        val |= values[1] << (Config::NUM_BITS_PER_BLOCK - wordOffset);
    val &= utils::bitMaskRange(0, size);

    return val;
}

template<class Config>
typename Config::BaseType BitVectorState<Config>::extractNonStraddling(typename Config::Plane plane, size_t start, size_t size) const
{
    HCL_ASSERT(start % Config::NUM_BITS_PER_BLOCK + size <= Config::NUM_BITS_PER_BLOCK);
    return utils::bitfieldExtract(m_values[plane][start / Config::NUM_BITS_PER_BLOCK], start % Config::NUM_BITS_PER_BLOCK, size);
}

template<class Config>
inline void BitVectorState<Config>::insert(typename Config::Plane plane, size_t offset, size_t size, typename Config::BaseType value)
{
    HCL_ASSERT(size <= Config::NUM_BITS_PER_BLOCK);
    const size_t wordOffset = offset % Config::NUM_BITS_PER_BLOCK;
    if (wordOffset + size <= Config::NUM_BITS_PER_BLOCK)
    {
        insertNonStraddling(plane, offset, size, value);
        return;
    }

    auto* dst = &m_values[plane][offset / Config::NUM_BITS_PER_BLOCK];
    dst[0] = utils::bitfieldInsert(dst[0], wordOffset, Config::NUM_BITS_PER_BLOCK - wordOffset, value);
    value >>= Config::NUM_BITS_PER_BLOCK - wordOffset;
    dst[1] = utils::bitfieldInsert(dst[1], 0, (wordOffset + size) % Config::NUM_BITS_PER_BLOCK, value);
}

template<class Config>
void BitVectorState<Config>::insertNonStraddling(typename Config::Plane plane, size_t start, size_t size, typename Config::BaseType value)
{
    HCL_ASSERT(start % Config::NUM_BITS_PER_BLOCK + size <= Config::NUM_BITS_PER_BLOCK);
    if (size)
    {
        auto& op = m_values[plane][start / Config::NUM_BITS_PER_BLOCK];
        op = utils::bitfieldInsert(op, start % Config::NUM_BITS_PER_BLOCK, size, value);
    }
}



}
