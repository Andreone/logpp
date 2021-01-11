#pragma once

#include "logpp/core/Clock.h"
#include "logpp/core/FormatArgs.h"
#include "logpp/core/LogBuffer.h"
#include "logpp/core/LogFieldVisitor.h"
#include "logpp/core/StringLiteral.h"

#include "logpp/utils/tuple.h"

#include <cstddef>

#include <fmt/format.h>

namespace logpp
{
    namespace details
    {

        template< typename T >
        using OffsetT = decltype(static_cast< LogBufferBase* >(nullptr)->write(std::declval< T >()));

        template<typename Arg>
        auto write(LogBufferBase& buffer, const Arg& arg)
        {
            auto keyOffset = buffer.write(arg.key);
            auto valueOffset = buffer.write(arg.value);

            return fieldOffset(keyOffset, valueOffset);
        }

        template< typename... Args >
        auto writeAll(LogBufferBase& buffer, Args&& ...args)
        {
            return std::make_tuple(write(buffer, args)...);
        }

        template<typename Tuple, size_t... Indexes>
        auto writeArgsImpl(LogBufferBase& buffer, const Tuple& args, std::index_sequence<Indexes...>)
        {
            return std::make_tuple(buffer.write(std::get<Indexes>(args))...);
        }

        template<typename...Args>
        auto writeArgs(LogBufferBase& buffer, const std::tuple<Args...>& args)
        {
            return writeArgsImpl(buffer, args, std::make_index_sequence<sizeof...(Args)>{});
        }

        template<typename> struct TextBlock;

        template<typename... Args>
        struct TextBlock<std::tuple<Args...>>
        {
            using ArgsOffsets = std::tuple<Args...>;

            TextBlock(StringOffset formatStrOffset, ArgsOffsets argsOffsets)
                : formatStrOffset { formatStrOffset }
                , argsOffsets { argsOffsets }
            {}

            void formatTo(fmt::memory_buffer& buffer, LogBufferView view) const
            {
                auto formatStr = formatStrOffset.get(view);
                formatImpl(view, buffer, formatStr, std::make_index_sequence<sizeof...(Args)>{});
            }

        private:
            StringOffset formatStrOffset;
            ArgsOffsets argsOffsets;

            template<size_t... Indexes>
            void formatImpl(LogBufferView view, fmt::memory_buffer& buffer, std::string_view formatStr, std::index_sequence<Indexes...>) const
            {
                fmt::format_to(buffer, formatStr, getArg<Indexes>(view)...);
            }

            template<size_t N>
            auto getArg(LogBufferView view) const
            {
                auto offset = std::get<N>(argsOffsets);
                return offset.get(view);
            }
        };

        template<typename> struct FieldsBlock;

        #pragma pack(push, 1)
        template<typename... Fields>
        struct FieldsBlock<std::tuple<Fields...>>
        {
            using Offsets = std::tuple<Fields...>;

            FieldsBlock(Offsets offsets)
                : offsets { offsets }
            {}

            void visit(LogBufferView view, LogFieldVisitor& visitor) const
            {
                tuple_utils::visit(offsets, [&](const auto& fieldOffset) {
                    auto key = fieldOffset.key.get(view);
                    auto value = fieldOffset.value.get(view);
                    visitor.visit(key, value);
                });
            }

            constexpr size_t count() const
            {
                return sizeof...(Fields);
            }

        private:
            Offsets offsets;
        };
        #pragma pack(pop)

        template<typename Offsets>
        FieldsBlock<Offsets> fieldsBlock(Offsets offsets)
        {
            return { offsets };
        }

        template<typename ArgsOffsets>
        TextBlock<ArgsOffsets> textBlock(StringOffset textOffset, ArgsOffsets argsOffsets)
        {
            return { textOffset, argsOffsets };
        }
    }

    template<typename KeyOffset, typename OffsetT>
    struct FieldOffset
    {
        KeyOffset key;
        OffsetT value;
    };

    template<typename KeyOffset, typename OffsetT>
    FieldOffset<KeyOffset, OffsetT> fieldOffset(KeyOffset key, OffsetT value)
    {
        return { key, value };
    }

    class EventLogBuffer : public LogBuffer<256>
    {
    public:
        using TextFormatFunc = void (*)(const LogBufferBase& buffer, uint16_t offsetsIndex, fmt::memory_buffer& formatBuf);
        using FieldsVisitFunc = void (*)(const LogBufferBase& buffer, uint16_t offsetsIndex, LogFieldVisitor& visitor);

        static constexpr size_t HeaderOffset = 0;

        struct Header
        {
            TimePoint timePoint;

            uint16_t textBlockIndex;
            TextFormatFunc formatFunc;

            uint8_t fieldsCount;
            uint16_t fieldsBlockIndex;
            FieldsVisitFunc fieldsVisitFunc;
        };

        EventLogBuffer()
        {
            decodeHeader()->fieldsCount = 0;
            advance(HeaderOffset + sizeof(Header));
        }

        void writeTime(TimePoint timePoint)
        {
            decodeHeader()->timePoint = timePoint;
        }

        template<typename Str>
        void writeText(const Str& str)
        {
            auto offset = this->write(str);

            auto block = details::textBlock(offset, std::make_tuple());
            auto blockOffset = this->encode(block);

            encodeTextBlock(block, blockOffset);
        }

        template<typename... Args>
        void writeText(const FormatArgsHolder<Args...>& holder)
        {
            auto formatStrOffset = this->write(holder.formatStr);
            auto argsOffsets = details::writeArgs(*this, holder.args);

            auto block = details::textBlock(formatStrOffset, argsOffsets);
            auto blockOffset = this->encode(block);

            encodeTextBlock(block, blockOffset);
        }

        template<typename... Fields>
        void writeFields(Fields&&... fields)
        {
            auto offsets = details::writeAll(*this, std::forward<Fields>(fields)...);

            auto block = details::fieldsBlock(offsets);
            auto blockOffset = this->encode(block);

            encodeFieldsBlock(block, blockOffset);
        }

        void visitFields(LogFieldVisitor& visitor) const
        {
            const auto* header = decodeHeader();
            if (header->fieldsCount > 0)
                std::invoke(header->fieldsVisitFunc, *this, header->fieldsBlockIndex, visitor);
        }

        TimePoint time() const
        {
            return decodeHeader()->timePoint;
        }

        void formatText(fmt::memory_buffer& buffer) const
        {
            const auto* header = decodeHeader();
            std::invoke(header->formatFunc, *this, header->textBlockIndex, buffer);
        }

    private:
        template<typename Block>
        void encodeTextBlock(const Block&, size_t blockIndex)
        {
            auto* header           = overlayAt<Header>(HeaderOffset);
            header->textBlockIndex = static_cast<uint16_t>(blockIndex);
            header->formatFunc     = [](const LogBufferBase& buffer, uint16_t blockIndex, fmt::memory_buffer& formatBuf)
            {
                LogBufferView view {buffer};
                const Block* block = view.overlayAs<Block>(blockIndex);
                block->formatTo(formatBuf, view);
            };
        }

        template<typename Block>
        void encodeFieldsBlock(const Block& block, size_t blockIndex)
        {
            auto* header             = overlayAt<Header>(HeaderOffset);
            header->fieldsCount      = static_cast<uint8_t>(block.count());
            header->fieldsBlockIndex = static_cast<uint16_t>(blockIndex);
            header->fieldsVisitFunc  = [](const LogBufferBase& buffer, uint16_t blockIndex, LogFieldVisitor& visitor)
            {
                LogBufferView view{buffer};
                const Block* block = view.overlayAs<Block>(blockIndex);

                visitor.visitStart(block->count());
                block->visit(view, visitor);
                visitor.visitEnd();
            };
        }

        Header* decodeHeader()
        {
            return overlayAt<Header>(HeaderOffset);
        }

        const Header* decodeHeader() const
        {
            return overlayAt<Header>(HeaderOffset);
        }
    };
}