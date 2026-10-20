#pragma once
#include "reactor/mysql_connection.hh"
#include <charconv>
#include <concepts>
#include <stdexcept>
#include <string_view>
#include <type_traits>

namespace imm::message_io
{
// 单行按值持有，避免从临时查询结果取得的行跨挂起点后悬空。
class sql_row
{
    std::vector<std::optional<std::string>> _columns;

  public:
    sql_row() = default;
    explicit sql_row(std::vector<std::optional<std::string>> columns) : _columns(std::move(columns)) {}
    explicit operator bool() const { return !_columns.empty(); }
    const std::optional<std::string>& operator[](size_t index) const { return _columns.at(index); }
};
struct sql_result : io::mysql_rows_result {
    explicit sql_result(io::mysql_rows_result result) : io::mysql_rows_result(std::move(result)) {}
    sql_row first_row() const { return rows.empty() ? sql_row{} : sql_row(rows.front()); }
};
template <typename Value>
    requires(std::integral<Value> || std::same_as<Value, std::string>)
Value field(const std::optional<std::string>& input)
{
    if (!input) {
        throw std::runtime_error("unexpected SQL NULL");
    }
    if constexpr (std::same_as<Value, std::string>) {
        return *input;
    } else {
        Value value{};
        auto result = std::from_chars(input->data(), input->data() + input->size(), value);
        if (result.ec != std::errc{} || result.ptr != input->data() + input->size()) {
            throw std::runtime_error("invalid SQL integer");
        }
        return value;
    }
}
template <typename Value>
concept sql_parameter = std::integral<std::remove_cvref_t<Value>> || std::convertible_to<Value, std::string_view>;
template <sql_parameter... Values> std::vector<io::mysql_parameter> parameters(Values&&... values)
{
    std::vector<io::mysql_parameter> result;
    result.reserve(sizeof...(values));
    auto add = [&result]<sql_parameter Value>(Value&& value) {
        if constexpr (std::integral<std::remove_cvref_t<Value>>) {
            result.emplace_back(static_cast<int64_t>(value));
        } else {
            result.emplace_back(std::string(std::string_view(value)));
        }
    };
    (add(std::forward<Values>(values)), ...);
    return result;
}
} // namespace imm::message_io
