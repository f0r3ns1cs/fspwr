#pragma once

// Small result type: either a value or a PowerError. C++20 has no
// std::expected, so this is the minimal subset we need.

#include <optional>
#include <utility>
#include <variant>

namespace pwr
{
	template <typename E> struct Unexpected {
		E error;
	};

	template <typename T, typename E> class Expected {
	  public:
		Expected(T value) : v_(std::in_place_index<0>, std::move(value)) {
		}
		Expected(Unexpected<E> unex) : v_(std::in_place_index<1>, std::move(unex.error)) {
		}

		bool hasValue() const {
			return v_.index() == 0;
		}
		explicit operator bool() const {
			return hasValue();
		}

		T& value() {
			return std::get<0>(v_);
		}
		const T& value() const {
			return std::get<0>(v_);
		}
		E& error() {
			return std::get<1>(v_);
		}
		const E& error() const {
			return std::get<1>(v_);
		}

		T& operator*() {
			return value();
		}
		const T& operator*() const {
			return value();
		}
		T* operator->() {
			return &value();
		}
		const T* operator->() const {
			return &value();
		}

	  private:
		std::variant<T, E> v_;
	};

	template <typename E> class Expected<void, E> {
	  public:
		Expected() = default;
		Expected(Unexpected<E> unex) : error_(std::move(unex.error)) {
		}

		bool hasValue() const {
			return !error_.has_value();
		}
		explicit operator bool() const {
			return hasValue();
		}

		E& error() {
			return *error_;
		}
		const E& error() const {
			return *error_;
		}

	  private:
		std::optional<E> error_;
	};

}
