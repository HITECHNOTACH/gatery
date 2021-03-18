#pragma once
#include <hcl/frontend.h>

#include <map>
#include <list>
#include <string_view>
#include <variant>
#include <iostream>

namespace hcl::stl
{
    struct AvalonMM
    {
        AvalonMM() = default;
        AvalonMM(AvalonMM&&) = default;
        AvalonMM(const AvalonMM&) = delete;
        void operator=(const AvalonMM&) = delete;

        BVec address;
        std::optional<Bit> ready;
        std::optional<Bit> read;
        std::optional<Bit> write;
        std::optional<BVec> writeData;
        std::optional<BVec> readData;
        std::optional<Bit> readDataValid;

        size_t readLatency = 0;
        size_t readyLatency = 0;

        std::map<std::string_view, Selection> addressSel;
        std::map<std::string_view, Selection> dataSel;

        void pinIn(std::string_view prefix);
        
        template<typename T>
        void connect(Memory<T>& mem, BitWidth dataWidth = 32_b);


        void createReadDataValid();
        void createReadLatency(size_t targetLatency);
    };

    class AvalonNetworkSection
    {
    public:
        AvalonNetworkSection(std::string name = "");

        void clear();
        void add(std::string name, AvalonMM port);
        AvalonNetworkSection& addSection(std::string name);

        AvalonMM& find(std::string_view path);

        void assignPins();
        AvalonMM demux();

    protected:
        std::string m_name;
        std::vector<std::pair<std::string, AvalonMM>> m_port;
        std::list<AvalonNetworkSection> m_subSections;
    };

    template<typename T>
    inline void AvalonMM::connect(Memory<T>& mem, BitWidth dataWidth)
    {
        struct SigInfo
        {
            std::string name;
            BVec* signalVec = nullptr;
            Bit* signalBit = nullptr;
        
            Selection from;
        };
        
        struct SigVis : CompoundNameVisitor
        {
            virtual void operator () (BVec& a) final
            {
                for (size_t i = 0; i < a.size(); i += regWidthLimit)
                {
                    SigInfo& sig = regMap.emplace_back().emplace_back();
                    sig.name = makeName();
                    sig.signalVec = &a;
                    sig.from = Selection::Slice(i, std::min<size_t>(regWidthLimit, a.size() - i));
                }
            }
        
            virtual void operator () (Bit& a) final
            {
                if (regMap.empty() ||
                    regMap.back().front().signalVec ||
                    currentRegWidth == regWidthLimit)
                {
                    regMap.emplace_back();
                    currentRegWidth = 0;
                }
        
                SigInfo& sig = regMap.back().emplace_back();
                sig.name = makeName();
                sig.signalBit = &a;
            }
        
            std::vector<std::vector<SigInfo>> regMap;
            size_t currentRegWidth = 0;
            size_t regWidthLimit = 1;
        };
        
        BVec memAddress = mem.addressWidth();
        HCL_NAMED(memAddress);
        
        auto&& port = mem[memAddress];
        T memContent = port.read();
        
        SigVis v;
        v.regWidthLimit = dataWidth.value;
        VisitCompound<T>{}(memContent, v);
        
        BitWidth regAddrWidth{ utils::Log2C(v.regMap.size()) };
        address = regAddrWidth + mem.addressWidth();
        memAddress = address(regAddrWidth.value, mem.addressWidth().value);
        
        write = Bit{};
        writeData = dataWidth;
        readData = ConstBVec(0, dataWidth.value);
        BVec regAddress = address(0, regAddrWidth.value);
        HCL_NAMED(regAddress);
        for (size_t r = 0; r < v.regMap.size(); ++r)
        {
            IF(regAddress == r)
            {
                auto& reg = v.regMap[r];
                if (reg.size() == 1 && reg.front().signalVec)
                {
                    SigInfo& sig = reg.front();
                    BVec& source = (*sig.signalVec)(sig.from);
                    readData = zext(source);
        
                    IF(*write)
                        source = (*writeData)(0, sig.from.width);
                }
                else
                {
                    for (size_t i = 0; i < reg.size(); ++i)
                    {
                        readData.value()[i] = *reg[i].signalBit;
                        (*reg[i].signalBit) = (*writeData)[i];
                    }
                }
            }
        }

        *readData = reg(*readData);
        
        IF(*write)
            port = memContent;
        
    }
}

BOOST_HANA_ADAPT_STRUCT(hcl::stl::AvalonMM, address, ready, read, write, writeData, readData, readDataValid, readLatency, readyLatency, addressSel, dataSel);
