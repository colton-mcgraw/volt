#include <model/part21.hpp>

#include <algorithm>
#include <bit>
#include <format>

namespace volt::model::part21
{

    // ---------------------------------------------------------------------
    // parameter
    // ---------------------------------------------------------------------

    kind parameter::type() const noexcept
    {
        return m_file != nullptr ? m_file->m_values[m_index].type : kind::omitted;
    }

    std::int64_t parameter::integer() const noexcept
    {
        if (!is(kind::integer))
            return 0;
        return static_cast<std::int64_t>(m_file->m_values[m_index].b);
    }

    double parameter::number() const noexcept
    {
        switch (type())
        {
        case kind::integer:
            return static_cast<double>(static_cast<std::int64_t>(m_file->m_values[m_index].b));
        case kind::real:
            return std::bit_cast<double>(m_file->m_values[m_index].b);
        default:
            return 0.0;
        }
    }

    std::string_view parameter::text() const noexcept
    {
        const kind t = type();
        if (t != kind::string && t != kind::enumeration && t != kind::binary)
            return {};
        const file::value &v = m_file->m_values[m_index];
        return std::string_view(m_file->m_text).substr(static_cast<std::size_t>(v.b), v.a);
    }

    std::uint64_t parameter::reference() const noexcept
    {
        return is(kind::reference) ? m_file->m_values[m_index].b : 0;
    }

    std::size_t parameter::size() const noexcept
    {
        return is(kind::list) ? m_file->m_values[m_index].a : 0;
    }

    parameter parameter::operator[](std::size_t index) const noexcept
    {
        if (index >= size())
            return {};
        return parameter(m_file, static_cast<std::uint32_t>(m_file->m_values[m_index].b + index));
    }

    std::string_view parameter::type_name() const noexcept
    {
        if (!is(kind::typed))
            return {};
        return m_file->name(m_file->m_values[m_index].a);
    }

    parameter parameter::value() const noexcept
    {
        parameter current = *this;
        // Typed values nest no deeper than the parser allows, but a file
        // assembled by hand could do anything; the bound costs nothing.
        for (int i = 0; i < 64 && current.is(kind::typed); ++i)
            current = parameter(m_file, static_cast<std::uint32_t>(m_file->m_values[current.m_index].b));
        return current;
    }

    parameter parameter::inner() const noexcept
    {
        if (!is(kind::typed))
            return {};
        return parameter(m_file, static_cast<std::uint32_t>(m_file->m_values[m_index].b));
    }

    bool parameter::logical(bool fallback) const noexcept
    {
        const std::string_view name = value().text();
        if (!value().is(kind::enumeration))
            return fallback;
        if (name == "T")
            return true;
        if (name == "F")
            return false;
        return fallback;
    }

    // ---------------------------------------------------------------------
    // entity
    // ---------------------------------------------------------------------

    namespace
    {
        constexpr std::uint32_t no_name = 0xffffffffu;
    } // namespace

    std::uint64_t entity::id() const noexcept
    {
        if (m_file == nullptr)
            return 0;
        return m_header ? 0 : m_file->m_instances[m_index].id;
    }

    std::uint32_t entity::line() const noexcept
    {
        if (m_file == nullptr)
            return 0;
        return (m_header ? m_file->m_header : m_file->m_instances)[m_index].line;
    }

    bool entity::is_complex() const noexcept
    {
        if (m_file == nullptr)
            return false;
        return (m_header ? m_file->m_header : m_file->m_instances)[m_index].complex;
    }

    std::size_t entity::part_count() const noexcept
    {
        if (m_file == nullptr)
            return 0;
        return (m_header ? m_file->m_header : m_file->m_instances)[m_index].part_count;
    }

    std::uint32_t entity::name_id(std::size_t part) const noexcept
    {
        if (part >= part_count())
            return no_name;
        const auto &record = (m_header ? m_file->m_header : m_file->m_instances)[m_index];
        return m_file->m_parts[record.first_part + part].name;
    }

    std::string_view entity::name(std::size_t part) const noexcept
    {
        const std::uint32_t id = name_id(part);
        return id == no_name ? std::string_view{} : m_file->name(id);
    }

    parameter entity::parameters(std::size_t part) const noexcept
    {
        if (part >= part_count())
            return {};
        const auto &record = (m_header ? m_file->m_header : m_file->m_instances)[m_index];
        return parameter(m_file, m_file->m_parts[record.first_part + part].parameters);
    }

    std::optional<std::size_t> entity::find_part(std::uint32_t id) const noexcept
    {
        const std::size_t count = part_count();
        for (std::size_t part = 0; part < count; ++part)
        {
            if (name_id(part) == id)
                return part;
        }
        return std::nullopt;
    }

    std::optional<std::size_t> entity::find_part(std::string_view type) const noexcept
    {
        if (m_file == nullptr)
            return std::nullopt;
        const auto id = m_file->name_id(type);
        if (!id)
            return std::nullopt;
        return find_part(*id);
    }

    // ---------------------------------------------------------------------
    // file
    // ---------------------------------------------------------------------

    std::optional<std::uint32_t> file::find(std::uint64_t id) const noexcept
    {
        if (!m_dense.empty() || m_sparse.empty())
        {
            if (id < m_dense.size() && m_dense[static_cast<std::size_t>(id)] != 0)
                return m_dense[static_cast<std::size_t>(id)] - 1;
            return std::nullopt;
        }
        const auto it = std::lower_bound(m_sparse.begin(), m_sparse.end(), id,
                                         [](const auto &entry, std::uint64_t key) { return entry.first < key; });
        if (it == m_sparse.end() || it->first != id)
            return std::nullopt;
        return it->second;
    }

    entity file::get(std::uint64_t id) const noexcept
    {
        const auto index = find(id);
        return index ? (*this)[*index] : entity{};
    }

    entity file::find_header(std::string_view type) const noexcept
    {
        for (std::size_t i = 0; i < m_header.size(); ++i)
        {
            if (header(i).name() == type)
                return header(i);
        }
        return {};
    }

    section file::get_section(std::size_t index) const noexcept
    {
        if (index >= m_sections.size())
            return {};
        const section_record &record = m_sections[index];
        section result;
        result.first = record.first;
        result.count = record.count;
        if (record.parameters != no_parameters)
            result.parameters = parameter(this, record.parameters);
        return result;
    }

    std::optional<std::uint32_t> file::name_id(std::string_view name) const noexcept
    {
        const auto it = m_name_index.find(name);
        if (it == m_name_index.end())
            return std::nullopt;
        return it->second;
    }

    std::string_view file::name(std::uint32_t id) const noexcept
    {
        return id < m_names.size() ? std::string_view(m_names[id]) : std::string_view{};
    }

    std::size_t file::memory_used() const noexcept
    {
        std::size_t total = m_values.capacity() * sizeof(value) + m_text.capacity() + m_parts.capacity() * sizeof(part) +
                            (m_instances.capacity() + m_header.capacity()) * sizeof(instance) +
                            m_sections.capacity() * sizeof(section_record) + m_dense.capacity() * sizeof(std::uint32_t) +
                            m_sparse.capacity() * sizeof(m_sparse[0]);
        for (const std::string &name : m_names)
            total += name.capacity() + sizeof(std::string) * 3;
        return total;
    }

    // ---------------------------------------------------------------------
    // builder
    // ---------------------------------------------------------------------

    builder::builder()
    {
        m_stack.reserve(256);
    }

    void builder::fail(std::string message)
    {
        if (m_error.empty())
            m_error = std::move(message);
    }

    std::uint32_t builder::intern(std::string_view name)
    {
        const auto it = m_file.m_name_index.find(name);
        if (it != m_file.m_name_index.end())
            return it->second;
        const auto id = static_cast<std::uint32_t>(m_file.m_names.size());
        m_file.m_names.emplace_back(name);
        m_file.m_name_index.emplace(std::string(name), id);
        return id;
    }

    void builder::push(file::value value)
    {
        if (m_frames.empty())
        {
            fail("a value outside of an entity");
            return;
        }
        m_stack.push_back(value);
    }

    std::uint32_t builder::commit(std::uint32_t start)
    {
        const auto first = static_cast<std::uint32_t>(m_file.m_values.size());
        m_file.m_values.insert(m_file.m_values.end(), m_stack.begin() + start, m_stack.end());
        m_stack.resize(start);
        return first;
    }

    void builder::begin_header(std::string_view type)
    {
        if (!m_frames.empty())
            fail("an entity begun inside another");
        m_pending = {};
        m_pending_header = true;
        m_pending_parts.clear();
        m_frames.push_back({frame_kind::entity, static_cast<std::uint32_t>(m_stack.size()), 0});
        begin_part(type);
    }

    void builder::begin_section()
    {
        if (!m_frames.empty())
            fail("a section begun inside an entity");
        file::section_record record;
        record.first = static_cast<std::uint32_t>(m_file.m_instances.size());
        m_file.m_sections.push_back(record);
    }

    void builder::begin_section_parameters()
    {
        if (!m_frames.empty() || m_file.m_sections.empty())
        {
            fail("section parameters outside of a section's start");
            return;
        }
        m_frames.push_back({frame_kind::section, static_cast<std::uint32_t>(m_stack.size()), 0});
    }

    void builder::end_section_parameters()
    {
        if (m_frames.empty() || m_frames.back().kind != frame_kind::section)
        {
            fail("end_section_parameters() without begin_section_parameters()");
            return;
        }
        const frame top = m_frames.back();
        m_frames.pop_back();
        const auto count = static_cast<std::uint32_t>(m_stack.size() - top.start);
        const std::uint32_t first = commit(top.start);
        m_file.m_values.push_back({kind::list, count, first});
        m_file.m_sections.back().parameters = static_cast<std::uint32_t>(m_file.m_values.size() - 1);
    }

    void builder::begin_instance(std::uint64_t id, std::uint32_t line, bool complex)
    {
        if (!m_frames.empty())
            fail("an instance begun inside another");
        if (m_file.m_sections.empty())
            begin_section();
        m_pending = {};
        m_pending.id = id;
        m_pending.line = line;
        m_pending.complex = complex;
        m_pending_header = false;
        m_pending_parts.clear();
        m_frames.push_back({frame_kind::entity, static_cast<std::uint32_t>(m_stack.size()), 0});
    }

    void builder::begin_part(std::string_view type)
    {
        if (m_frames.empty() || m_frames.back().kind != frame_kind::entity)
        {
            fail("a part outside of an instance");
            return;
        }
        m_frames.push_back({frame_kind::part, static_cast<std::uint32_t>(m_stack.size()), intern(type)});
    }

    void builder::end_part()
    {
        if (m_frames.empty() || m_frames.back().kind != frame_kind::part)
        {
            fail("end_part() without begin_part()");
            return;
        }
        const frame top = m_frames.back();
        m_frames.pop_back();
        const auto count = static_cast<std::uint32_t>(m_stack.size() - top.start);
        const std::uint32_t first = commit(top.start);
        m_file.m_values.push_back({kind::list, count, first});
        m_pending_parts.push_back({top.name, static_cast<std::uint32_t>(m_file.m_values.size() - 1)});
    }

    void builder::end_entity()
    {
        if (!m_frames.empty() && m_frames.back().kind == frame_kind::part)
            end_part();
        if (m_frames.empty() || m_frames.back().kind != frame_kind::entity)
        {
            fail("end_entity() without an entity begun");
            return;
        }
        m_frames.pop_back();
        if (m_pending_parts.empty() || m_pending_parts.size() > 0xffff)
        {
            fail(std::format("line {}: an instance needs between 1 and 65535 types", m_pending.line));
            return;
        }

        m_pending.first_part = static_cast<std::uint32_t>(m_file.m_parts.size());
        m_pending.part_count = static_cast<std::uint16_t>(m_pending_parts.size());
        m_file.m_parts.insert(m_file.m_parts.end(), m_pending_parts.begin(), m_pending_parts.end());
        if (m_pending_header)
            m_file.m_header.push_back(m_pending);
        else
        {
            m_file.m_instances.push_back(m_pending);
            ++m_file.m_sections.back().count;
        }
    }

    void builder::omitted()
    {
        push({kind::omitted, 0, 0});
    }

    void builder::derived()
    {
        push({kind::derived, 0, 0});
    }

    void builder::integer(std::int64_t value)
    {
        push({kind::integer, 0, static_cast<std::uint64_t>(value)});
    }

    void builder::real(double value)
    {
        push({kind::real, 0, std::bit_cast<std::uint64_t>(value)});
    }

    void builder::string(std::string_view utf8)
    {
        const std::uint64_t offset = m_file.m_text.size();
        m_file.m_text.append(utf8);
        push({kind::string, static_cast<std::uint32_t>(utf8.size()), offset});
    }

    void builder::enumeration(std::string_view name)
    {
        const std::uint64_t offset = m_file.m_text.size();
        m_file.m_text.append(name);
        push({kind::enumeration, static_cast<std::uint32_t>(name.size()), offset});
    }

    void builder::binary(std::string_view hex)
    {
        const std::uint64_t offset = m_file.m_text.size();
        m_file.m_text.append(hex);
        push({kind::binary, static_cast<std::uint32_t>(hex.size()), offset});
    }

    void builder::reference(std::uint64_t id)
    {
        push({kind::reference, 0, id});
    }

    void builder::begin_list()
    {
        if (m_frames.empty())
        {
            fail("a list outside of an entity");
            return;
        }
        m_frames.push_back({frame_kind::list, static_cast<std::uint32_t>(m_stack.size()), 0});
    }

    void builder::end_list()
    {
        if (m_frames.empty() || m_frames.back().kind != frame_kind::list)
        {
            fail("end_list() without begin_list()");
            return;
        }
        const frame top = m_frames.back();
        m_frames.pop_back();
        const auto count = static_cast<std::uint32_t>(m_stack.size() - top.start);
        const std::uint32_t first = commit(top.start);
        m_stack.push_back({kind::list, count, first});
    }

    void builder::begin_typed(std::string_view type)
    {
        if (m_frames.empty())
        {
            fail("a typed value outside of an entity");
            return;
        }
        m_frames.push_back({frame_kind::typed, static_cast<std::uint32_t>(m_stack.size()), intern(type)});
    }

    void builder::end_typed()
    {
        if (m_frames.empty() || m_frames.back().kind != frame_kind::typed)
        {
            fail("end_typed() without begin_typed()");
            return;
        }
        const frame top = m_frames.back();
        m_frames.pop_back();
        if (m_stack.size() - top.start != 1)
        {
            fail("a typed value must hold exactly one value");
            m_stack.resize(top.start);
            m_stack.push_back({kind::omitted, 0, 0});
            return;
        }
        const std::uint32_t first = commit(top.start);
        m_stack.push_back({kind::typed, top.name, first});
    }

    std::expected<file, std::string> builder::finish()
    {
        if (!m_frames.empty())
            fail("finish() with an entity still open");
        if (!m_error.empty())
            return std::unexpected(m_error);
        if (m_file.m_values.size() >= 0xfffffff0u || m_file.m_text.size() >= 0xfffffff0u)
            return std::unexpected("the file is too large (over 4 billion values)");

        file &f = m_file;
        std::uint64_t largest = 0;
        for (const file::instance &instance : f.m_instances)
            largest = std::max(largest, instance.id);

        const std::size_t count = f.m_instances.size();
        const auto duplicate = [&](std::uint32_t index) {
            const file::instance &instance = f.m_instances[index];
            return std::unexpected(std::format("line {}: #{} is defined twice", instance.line, instance.id));
        };

        if (largest <= 16 * static_cast<std::uint64_t>(count) + 4096)
        {
            f.m_dense.assign(static_cast<std::size_t>(largest) + 1, 0);
            for (std::uint32_t index = 0; index < count; ++index)
            {
                std::uint32_t &slot = f.m_dense[static_cast<std::size_t>(f.m_instances[index].id)];
                if (slot != 0)
                    return duplicate(index);
                slot = index + 1;
            }
        }
        else
        {
            f.m_sparse.reserve(count);
            for (std::uint32_t index = 0; index < count; ++index)
                f.m_sparse.emplace_back(f.m_instances[index].id, index);
            std::sort(f.m_sparse.begin(), f.m_sparse.end());
            for (std::size_t i = 1; i < f.m_sparse.size(); ++i)
            {
                if (f.m_sparse[i].first == f.m_sparse[i - 1].first)
                    return duplicate(std::max(f.m_sparse[i].second, f.m_sparse[i - 1].second));
            }
        }

        f.m_values.shrink_to_fit();
        f.m_parts.shrink_to_fit();
        f.m_instances.shrink_to_fit();
        file result = std::move(m_file);
        m_file = file{};
        return result;
    }

} // namespace volt::model::part21
