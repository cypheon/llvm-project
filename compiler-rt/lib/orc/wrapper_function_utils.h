//===-- wrapper_function_utils.h - Utilities for wrapper funcs --*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file is a part of the ORC runtime support library.
//
// The behavior of the utilities in this header must be synchronized with the
// behavior of the utilities in
// llvm/ExecutionEngine/Orc/Shared/WrapperFunctionUtils.h.
//
// The Simple Packed Serialization (SPS) utilities are used to generate
// argument and return buffers for wrapper functions using the following
// serialization scheme:
//
// Primitives:
//   bool, char, int8_t, uint8_t -- Two's complement 8-bit (0=false, 1=true)
//   int16_t, uint16_t           -- Two's complement 16-bit little endian
//   int32_t, uint32_t           -- Two's complement 32-bit little endian
//   int64_t, int64_t            -- Two's complement 64-bit little endian
//
// Sequence<T>:
//   Serialized as the sequence length (as a uint64_t) followed by the
//   serialization of each of the elements without padding.
//
// Tuple<T1, ..., TN>:
//   Serialized as each of the element types from T1 to TN without padding.
//
//===----------------------------------------------------------------------===//

#ifndef ORC_RT_WRAPPER_FUNCTION_UTILS_H
#define ORC_RT_WRAPPER_FUNCTION_UTILS_H

#include "adt.h"
#include "c_api.h"
#include "common.h"
#include "endian.h"
#include "error.h"
#include "stl_extras.h"

#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace __orc_rt {

/// C++ wrapper function result: Same as CWrapperFunctionResult but
/// auto-releases memory.
class WrapperFunctionResult {
public:
  /// Create a default WrapperFunctionResult.
  WrapperFunctionResult() { __orc_rt_CWrapperFunctionResultInit(&R); }

  /// Create a WrapperFunctionResult from a CWrapperFunctionResult. This
  /// instance takes ownership of the result object and will automatically
  /// call dispose on the result upon destruction.
  WrapperFunctionResult(__orc_rt_CWrapperFunctionResult R) : R(R) {}

  WrapperFunctionResult(const WrapperFunctionResult &) = delete;
  WrapperFunctionResult &operator=(const WrapperFunctionResult &) = delete;

  WrapperFunctionResult(WrapperFunctionResult &&Other) {
    __orc_rt_CWrapperFunctionResultInit(&R);
    std::swap(R, Other.R);
  }

  WrapperFunctionResult &operator=(WrapperFunctionResult &&Other) {
    __orc_rt_CWrapperFunctionResult Tmp;
    __orc_rt_CWrapperFunctionResultInit(&Tmp);
    std::swap(Tmp, Other.R);
    std::swap(R, Tmp);
    return *this;
  }

  ~WrapperFunctionResult() { __orc_rt_DisposeCWrapperFunctionResult(&R); }

  /// Relinquish ownership of and return the
  /// __orc_rt_CWrapperFunctionResult.
  __orc_rt_CWrapperFunctionResult release() {
    __orc_rt_CWrapperFunctionResult Tmp;
    __orc_rt_CWrapperFunctionResultInit(&Tmp);
    std::swap(R, Tmp);
    return Tmp;
  }

  /// Get an ArrayRef covering the data in the result.
  const char *data() const { return __orc_rt_CWrapperFunctionResultData(&R); }

  /// Returns the size of the data contained in this instance.
  size_t size() const { return __orc_rt_CWrapperFunctionResultSize(&R); }

  /// Returns true if this value is equivalent to a default-constructed
  /// WrapperFunctionResult.
  bool empty() const { return __orc_rt_CWrapperFunctionResultEmpty(&R); }

  /// Create a WrapperFunctionResult with the given size and return a pointer
  /// to the underlying memory.
  static char *allocate(WrapperFunctionResult &R, size_t Size) {
    __orc_rt_DisposeCWrapperFunctionResult(&R.R);
    __orc_rt_CWrapperFunctionResultInit(&R.R);
    return __orc_rt_CWrapperFunctionResultAllocate(&R.R, Size);
  }

  /// Copy from the given char range.
  static WrapperFunctionResult copyFrom(const char *Source, size_t Size) {
    return __orc_rt_CreateCWrapperFunctionResultFromRange(Source, Size);
  }

  /// Copy from the given null-terminated string (includes the null-terminator).
  static WrapperFunctionResult copyFrom(const char *Source) {
    return __orc_rt_CreateCWrapperFunctionResultFromString(Source);
  }

  /// Copy from the given std::string (includes the null terminator).
  static WrapperFunctionResult copyFrom(const std::string &Source) {
    return copyFrom(Source.c_str());
  }

  /// Create an out-of-band error by copying the given string.
  static WrapperFunctionResult createOutOfBandError(const char *Msg) {
    return __orc_rt_CreateCWrapperFunctionResultFromOutOfBandError(Msg);
  }

  /// If this value is an out-of-band error then this returns the error message,
  /// otherwise returns nullptr.
  const char *getOutOfBandError() const {
    return __orc_rt_CWrapperFunctionResultGetOutOfBandError(&R);
  }

private:
  __orc_rt_CWrapperFunctionResult R;
};

/// Output char buffer with overflow check.
class SPSOutputBuffer {
public:
  SPSOutputBuffer(char *Buffer, size_t Remaining)
      : Buffer(Buffer), Remaining(Remaining) {}
  bool write(const char *Data, size_t Size) {
    if (Size > Remaining)
      return false;
    memcpy(Buffer, Data, Size);
    Buffer += Size;
    Remaining -= Size;
    return true;
  }

private:
  char *Buffer = nullptr;
  size_t Remaining = 0;
};

/// Input char buffer with underflow check.
class SPSInputBuffer {
public:
  SPSInputBuffer() = default;
  SPSInputBuffer(const char *Buffer, size_t Remaining)
      : Buffer(Buffer), Remaining(Remaining) {}
  bool read(char *Data, size_t Size) {
    if (Size > Remaining)
      return false;
    memcpy(Data, Buffer, Size);
    Buffer += Size;
    Remaining -= Size;
    return true;
  }

  const char *data() const { return Buffer; }
  bool skip(size_t Size) {
    if (Size > Remaining)
      return false;
    Remaining -= Size;
    return true;
  }

private:
  const char *Buffer = nullptr;
  size_t Remaining = 0;
};

/// Specialize to describe how to serialize/deserialize to/from the given
/// concrete type.
template <typename SPSTagT, typename ConcreteT, typename _ = void>
class SPSSerializationTraits;

/// A utility class for serializing to a blob from a variadic list.
template <typename... ArgTs> class SPSArgList;

// Empty list specialization for SPSArgList.
template <> class SPSArgList<> {
public:
  static size_t size() { return 0; }

  static bool serialize(SPSOutputBuffer &OB) { return true; }
  static bool deserialize(SPSInputBuffer &IB) { return true; }

  static bool toWrapperFunctionResult(WrapperFunctionResult &R) {
    R = WrapperFunctionResult();
    return true;
  }
};

// Non-empty list specialization for SPSArgList.
template <typename SPSTagT, typename... SPSTagTs>
class SPSArgList<SPSTagT, SPSTagTs...> {
public:
  template <typename ArgT, typename... ArgTs>
  static size_t size(const ArgT &Arg, const ArgTs &...Args) {
    return SPSSerializationTraits<SPSTagT, ArgT>::size(Arg) +
           SPSArgList<SPSTagTs...>::size(Args...);
  }

  template <typename ArgT, typename... ArgTs>
  static bool serialize(SPSOutputBuffer &OB, const ArgT &Arg,
                        const ArgTs &...Args) {
    return SPSSerializationTraits<SPSTagT, ArgT>::serialize(OB, Arg) &&
           SPSArgList<SPSTagTs...>::serialize(OB, Args...);
  }

  template <typename ArgT, typename... ArgTs>
  static bool deserialize(SPSInputBuffer &IB, ArgT &Arg, ArgTs &...Args) {
    return SPSSerializationTraits<SPSTagT, ArgT>::deserialize(IB, Arg) &&
           SPSArgList<SPSTagTs...>::deserialize(IB, Args...);
  }

  template <typename... ArgTs>
  static bool toWrapperFunctionResult(WrapperFunctionResult &R,
                                      const ArgTs &...Args) {
    WrapperFunctionResult TR;
    char *DataPtr = WrapperFunctionResult::allocate(TR, size(Args...));

    SPSOutputBuffer OB(DataPtr, TR.size());
    if (!serialize(OB, Args...))
      return false;

    R = std::move(TR);
    return true;
  }

  template <typename... ArgTs>
  static bool fromBuffer(const char *Data, size_t Size, ArgTs &...Args) {
    SPSInputBuffer IB(Data, Size);
    return deserialize(IB, Args...);
  }
};

/// SPS serialization for integral types, bool, and char.
template <typename SPSTagT>
class SPSSerializationTraits<
    SPSTagT, SPSTagT,
    std::enable_if_t<std::is_same<SPSTagT, bool>::value ||
                     std::is_same<SPSTagT, char>::value ||
                     std::is_same<SPSTagT, int8_t>::value ||
                     std::is_same<SPSTagT, int16_t>::value ||
                     std::is_same<SPSTagT, int32_t>::value ||
                     std::is_same<SPSTagT, int64_t>::value ||
                     std::is_same<SPSTagT, uint8_t>::value ||
                     std::is_same<SPSTagT, uint16_t>::value ||
                     std::is_same<SPSTagT, uint32_t>::value ||
                     std::is_same<SPSTagT, uint64_t>::value>> {
public:
  static size_t size(const SPSTagT &Value) { return sizeof(SPSTagT); }

  static bool serialize(SPSOutputBuffer &OB, const SPSTagT &Value) {
    SPSTagT Tmp = Value;
    if (IsBigEndianHost)
      swapByteOrder(Tmp);
    return OB.write(reinterpret_cast<const char *>(&Tmp), sizeof(Tmp));
  }

  static bool deserialize(SPSInputBuffer &IB, SPSTagT &Value) {
    SPSTagT Tmp;
    if (!IB.read(reinterpret_cast<char *>(&Tmp), sizeof(Tmp)))
      return false;
    if (IsBigEndianHost)
      swapByteOrder(Tmp);
    Value = Tmp;
    return true;
  }
};

// Any empty placeholder suitable as a substitute for void when deserializing
class SPSEmpty {};

/// SPS tag type for target addresses.
///
/// SPSTagTargetAddresses should be serialized as a uint64_t value.
class SPSTagTargetAddress;

template <>
class SPSSerializationTraits<SPSTagTargetAddress, uint64_t>
    : public SPSSerializationTraits<uint64_t, uint64_t> {};

/// SPS tag type for tuples.
///
/// A blob tuple should be serialized by serializing each of the elements in
/// sequence.
template <typename... SPSTagTs> class SPSTuple {
public:
  /// Convenience typedef of the corresponding arg list.
  typedef SPSArgList<SPSTagTs...> AsArgList;
};

/// SPS tag type for sequences.
///
/// SPSSequences should be serialized as a uint64_t sequence length,
/// followed by the serialization of each of the elements.
template <typename SPSElementTagT> class SPSSequence;

/// SPS tag type for strings, which are equivalent to sequences of chars.
using SPSString = SPSSequence<char>;

/// SPS tag type for maps.
///
/// SPS maps are just sequences of (Key, Value) tuples.
template <typename SPSTagT1, typename SPSTagT2>
using SPSMap = SPSSequence<SPSTuple<SPSTagT1, SPSTagT2>>;

/// Serialization for SPSEmpty type.
template <> class SPSSerializationTraits<SPSEmpty, SPSEmpty> {
public:
  static size_t size(const SPSEmpty &EP) { return 0; }
  static bool serialize(SPSOutputBuffer &OB, const SPSEmpty &BE) {
    return true;
  }
  static bool deserialize(SPSInputBuffer &IB, SPSEmpty &BE) { return true; }
};

/// Specialize this to implement 'trivial' sequence serialization for
/// a concrete sequence type.
///
/// Trivial sequence serialization uses the sequence's 'size' member to get the
/// length of the sequence, and uses a range-based for loop to iterate over the
/// elements.
///
/// Specializing this template class means that you do not need to provide a
/// specialization of SPSSerializationTraits for your type.
template <typename SPSElementTagT, typename ConcreteSequenceT>
class TrivialSPSSequenceSerialization {
public:
  static constexpr bool available = false;
};

/// Specialize this to implement 'trivial' sequence deserialization for
/// a concrete sequence type.
///
/// Trivial deserialization calls a static 'reserve(SequenceT&)' method on your
/// specialization (you must implement this) to reserve space, and then calls
/// a static 'append(SequenceT&, ElementT&) method to append each of the
/// deserialized elements.
///
/// Specializing this template class means that you do not need to provide a
/// specialization of SPSSerializationTraits for your type.
template <typename SPSElementTagT, typename ConcreteSequenceT>
class TrivialSPSSequenceDeserialization {
public:
  static constexpr bool available = false;
};

/// Trivial std::string -> SPSSequence<char> serialization.
template <> class TrivialSPSSequenceSerialization<char, std::string> {
public:
  static constexpr bool available = true;
};

/// Trivial SPSSequence<char> -> std::string deserialization.
template <> class TrivialSPSSequenceDeserialization<char, std::string> {
public:
  static constexpr bool available = true;

  using element_type = char;

  static void reserve(std::string &S, uint64_t Size) { S.reserve(Size); }
  static bool append(std::string &S, char C) {
    S.push_back(C);
    return true;
  }
};

/// Trivial std::vector<T> -> SPSSequence<SPSElementTagT> serialization.
template <typename SPSElementTagT, typename T>
class TrivialSPSSequenceSerialization<SPSElementTagT, std::vector<T>> {
public:
  static constexpr bool available = true;
};

/// Trivial SPSSequence<SPSElementTagT> -> std::vector<T> deserialization.
template <typename SPSElementTagT, typename T>
class TrivialSPSSequenceDeserialization<SPSElementTagT, std::vector<T>> {
public:
  static constexpr bool available = true;

  using element_type = typename std::vector<T>::value_type;

  static void reserve(std::vector<T> &V, uint64_t Size) { V.reserve(Size); }
  static bool append(std::vector<T> &V, T E) {
    V.push_back(std::move(E));
    return true;
  }
};

/// 'Trivial' sequence serialization: Sequence is serialized as a uint64_t size
/// followed by a for-earch loop over the elements of the sequence to serialize
/// each of them.
template <typename SPSElementTagT, typename SequenceT>
class SPSSerializationTraits<SPSSequence<SPSElementTagT>, SequenceT,
                             std::enable_if_t<TrivialSPSSequenceSerialization<
                                 SPSElementTagT, SequenceT>::available>> {
public:
  static size_t size(const SequenceT &S) {
    size_t Size = SPSArgList<uint64_t>::size(static_cast<uint64_t>(S.size()));
    for (const auto &E : S)
      Size += SPSArgList<SPSElementTagT>::size(E);
    return Size;
  }

  static bool serialize(SPSOutputBuffer &OB, const SequenceT &S) {
    if (!SPSArgList<uint64_t>::serialize(OB, static_cast<uint64_t>(S.size())))
      return false;
    for (const auto &E : S)
      if (!SPSArgList<SPSElementTagT>::serialize(OB, E))
        return false;
    return true;
  }

  static bool deserialize(SPSInputBuffer &IB, SequenceT &S) {
    using TBSD = TrivialSPSSequenceDeserialization<SPSElementTagT, SequenceT>;
    uint64_t Size;
    if (!SPSArgList<uint64_t>::deserialize(IB, Size))
      return false;
    TBSD::reserve(S, Size);
    for (size_t I = 0; I != Size; ++I) {
      typename TBSD::element_type E;
      if (!SPSArgList<SPSElementTagT>::deserialize(IB, E))
        return false;
      if (!TBSD::append(S, std::move(E)))
        return false;
    }
    return true;
  }
};

/// SPSTuple serialization for std::pair.
template <typename SPSTagT1, typename SPSTagT2, typename T1, typename T2>
class SPSSerializationTraits<SPSTuple<SPSTagT1, SPSTagT2>, std::pair<T1, T2>> {
public:
  static size_t size(const std::pair<T1, T2> &P) {
    return SPSArgList<SPSTagT1>::size(P.first) +
           SPSArgList<SPSTagT2>::size(P.second);
  }

  static bool serialize(SPSOutputBuffer &OB, const std::pair<T1, T2> &P) {
    return SPSArgList<SPSTagT1>::serialize(OB, P.first) &&
           SPSArgList<SPSTagT2>::serialize(OB, P.second);
  }

  static bool deserialize(SPSInputBuffer &IB, std::pair<T1, T2> &P) {
    return SPSArgList<SPSTagT1>::deserialize(IB, P.first) &&
           SPSArgList<SPSTagT2>::deserialize(IB, P.second);
  }
};

/// Serialization for string_views.
///
/// Serialization is as for regular strings. Deserialization points directly
/// into the blob.
template <> class SPSSerializationTraits<SPSString, __orc_rt::string_view> {
public:
  static size_t size(const __orc_rt::string_view &S) {
    return SPSArgList<uint64_t>::size(static_cast<uint64_t>(S.size())) +
           S.size();
  }

  static bool serialize(SPSOutputBuffer &OB, const __orc_rt::string_view &S) {
    if (!SPSArgList<uint64_t>::serialize(OB, static_cast<uint64_t>(S.size())))
      return false;
    return OB.write(S.data(), S.size());
  }

  static bool deserialize(SPSInputBuffer &IB, __orc_rt::string_view &S) {
    const char *Data = nullptr;
    uint64_t Size;
    if (!SPSArgList<uint64_t>::deserialize(IB, Size))
      return false;
    Data = IB.data();
    if (!IB.skip(Size))
      return false;
    S = {Data, Size};
    return true;
  }
};

/// SPS tag type for errors.
class SPSError;

/// SPS tag type for expecteds, which are either a T or a string representing
/// an error.
template <typename SPSTagT> class SPSExpected;

namespace detail {

/// Helper type for serializing Errors.
///
/// llvm::Errors are move-only, and not inspectable except by consuming them.
/// This makes them unsuitable for direct serialization via
/// SPSSerializationTraits, which needs to inspect values twice (once to
/// determine the amount of space to reserve, and then again to serialize).
///
/// The WrapperFunctionSerializableError type is a helper that can be
/// constructed from an llvm::Error, but inspected more than once.
struct SPSSerializableError {
  bool HasError = false;
  std::string ErrMsg;
};

/// Helper type for serializing Expected<T>s.
///
/// See SPSSerializableError for more details.
///
// FIXME: Use std::variant for storage once we have c++17.
template <typename T> struct SPSSerializableExpected {
  bool HasValue = false;
  T Value{};
  std::string ErrMsg;
};

inline SPSSerializableError toSPSSerializable(Error Err) {
  if (Err)
    return {true, toString(std::move(Err))};
  return {false, {}};
}

inline Error fromSPSSerializable(SPSSerializableError BSE) {
  if (BSE.HasError)
    return make_error<StringError>(BSE.ErrMsg);
  return Error::success();
}

template <typename T>
SPSSerializableExpected<T> toSPSSerializable(Expected<T> E) {
  if (E)
    return {true, std::move(*E), {}};
  else
    return {false, {}, toString(E.takeError())};
}

template <typename T>
Expected<T> fromSPSSerializable(SPSSerializableExpected<T> BSE) {
  if (BSE.HasValue)
    return std::move(BSE.Value);
  else
    return make_error<StringError>(BSE.ErrMsg);
}

} // end namespace detail

/// Serialize to a SPSError from a detail::SPSSerializableError.
template <>
class SPSSerializationTraits<SPSError, detail::SPSSerializableError> {
public:
  static size_t size(const detail::SPSSerializableError &BSE) {
    size_t Size = SPSArgList<bool>::size(BSE.HasError);
    if (BSE.HasError)
      Size += SPSArgList<SPSString>::size(BSE.ErrMsg);
    return Size;
  }

  static bool serialize(SPSOutputBuffer &OB,
                        const detail::SPSSerializableError &BSE) {
    if (!SPSArgList<bool>::serialize(OB, BSE.HasError))
      return false;
    if (BSE.HasError)
      if (!SPSArgList<SPSString>::serialize(OB, BSE.ErrMsg))
        return false;
    return true;
  }

  static bool deserialize(SPSInputBuffer &IB,
                          detail::SPSSerializableError &BSE) {
    if (!SPSArgList<bool>::deserialize(IB, BSE.HasError))
      return false;

    if (!BSE.HasError)
      return true;

    return SPSArgList<SPSString>::deserialize(IB, BSE.ErrMsg);
  }
};

/// Serialize to a SPSExpected<SPSTagT> from a
/// detail::SPSSerializableExpected<T>.
template <typename SPSTagT, typename T>
class SPSSerializationTraits<SPSExpected<SPSTagT>,
                             detail::SPSSerializableExpected<T>> {
public:
  static size_t size(const detail::SPSSerializableExpected<T> &BSE) {
    size_t Size = SPSArgList<bool>::size(BSE.HasValue);
    if (BSE.HasValue)
      Size += SPSArgList<SPSTagT>::size(BSE.Value);
    else
      Size += SPSArgList<SPSString>::size(BSE.ErrMsg);
    return Size;
  }

  static bool serialize(SPSOutputBuffer &OB,
                        const detail::SPSSerializableExpected<T> &BSE) {
    if (!SPSArgList<bool>::serialize(OB, BSE.HasValue))
      return false;

    if (BSE.HasValue)
      return SPSArgList<SPSTagT>::serialize(OB, BSE.Value);

    return SPSArgList<SPSString>::serialize(OB, BSE.ErrMsg);
  }

  static bool deserialize(SPSInputBuffer &IB,
                          detail::SPSSerializableExpected<T> &BSE) {
    if (!SPSArgList<bool>::deserialize(IB, BSE.HasValue))
      return false;

    if (BSE.HasValue)
      return SPSArgList<SPSTagT>::deserialize(IB, BSE.Value);

    return SPSArgList<SPSString>::deserialize(IB, BSE.ErrMsg);
  }
};

/// Serialize to a SPSExpected<SPSTagT> from a detail::SPSSerializableError.
template <typename SPSTagT>
class SPSSerializationTraits<SPSExpected<SPSTagT>,
                             detail::SPSSerializableError> {
public:
  static size_t size(const detail::SPSSerializableError &BSE) {
    assert(BSE.HasError && "Cannot serialize expected from a success value");
    return SPSArgList<bool>::size(false) +
           SPSArgList<SPSString>::size(BSE.ErrMsg);
  }

  static bool serialize(SPSOutputBuffer &OB,
                        const detail::SPSSerializableError &BSE) {
    assert(BSE.HasError && "Cannot serialize expected from a success value");
    if (!SPSArgList<bool>::serialize(OB, false))
      return false;
    return SPSArgList<SPSString>::serialize(OB, BSE.ErrMsg);
  }
};

/// Serialize to a SPSExpected<SPSTagT> from a T.
template <typename SPSTagT, typename T>
class SPSSerializationTraits<SPSExpected<SPSTagT>, T> {
public:
  static size_t size(const T &Value) {
    return SPSArgList<bool>::size(true) + SPSArgList<SPSTagT>::size(Value);
  }

  static bool serialize(SPSOutputBuffer &OB, const T &Value) {
    if (!SPSArgList<bool>::serialize(OB, true))
      return false;
    return SPSArgList<SPSTagT>::serialize(Value);
  }
};

namespace detail {

template <typename WrapperFunctionImplT,
          template <typename> class ResultSerializer, typename... SPSTagTs>
class WrapperFunctionHandlerHelper
    : public WrapperFunctionHandlerHelper<
          decltype(&std::remove_reference_t<WrapperFunctionImplT>::operator()),
          ResultSerializer, SPSTagTs...> {};

template <typename RetT, typename... ArgTs,
          template <typename> class ResultSerializer, typename... SPSTagTs>
class WrapperFunctionHandlerHelper<RetT(ArgTs...), ResultSerializer,
                                   SPSTagTs...> {
public:
  using ArgTuple = std::tuple<std::decay_t<ArgTs>...>;
  using ArgIndices = std::make_index_sequence<std::tuple_size<ArgTuple>::value>;

  template <typename HandlerT>
  static WrapperFunctionResult apply(HandlerT &&H, const char *ArgData,
                                     size_t ArgSize) {
    ArgTuple Args;
    if (!deserialize(ArgData, ArgSize, Args, ArgIndices{}))
      return WrapperFunctionResult::createOutOfBandError(
          "Could not deserialize arguments for wrapper function call");

    return ResultSerializer<RetT>::serialize(
        call(std::forward<HandlerT>(H), Args, ArgIndices{}));
  }

private:
  template <std::size_t... I>
  static bool deserialize(const char *ArgData, size_t ArgSize, ArgTuple &Args,
                          std::index_sequence<I...>) {
    SPSInputBuffer IB(ArgData, ArgSize);
    return SPSArgList<SPSTagTs...>::deserialize(IB, std::get<I>(Args)...);
  }

  template <typename HandlerT, std::size_t... I>
  static decltype(auto) call(HandlerT &&H, ArgTuple &Args,
                             std::index_sequence<I...>) {
    return std::forward<HandlerT>(H)(std::get<I>(Args)...);
  }
};

// Map function references to function types.
template <typename RetT, typename... ArgTs,
          template <typename> class ResultSerializer, typename... SPSTagTs>
class WrapperFunctionHandlerHelper<RetT (&)(ArgTs...), ResultSerializer,
                                   SPSTagTs...>
    : public WrapperFunctionHandlerHelper<RetT(ArgTs...), ResultSerializer,
                                          SPSTagTs...> {};

// Map non-const member function types to function types.
template <typename ClassT, typename RetT, typename... ArgTs,
          template <typename> class ResultSerializer, typename... SPSTagTs>
class WrapperFunctionHandlerHelper<RetT (ClassT::*)(ArgTs...), ResultSerializer,
                                   SPSTagTs...>
    : public WrapperFunctionHandlerHelper<RetT(ArgTs...), ResultSerializer,
                                          SPSTagTs...> {};

// Map const member function types to function types.
template <typename ClassT, typename RetT, typename... ArgTs,
          template <typename> class ResultSerializer, typename... SPSTagTs>
class WrapperFunctionHandlerHelper<RetT (ClassT::*)(ArgTs...) const,
                                   ResultSerializer, SPSTagTs...>
    : public WrapperFunctionHandlerHelper<RetT(ArgTs...), ResultSerializer,
                                          SPSTagTs...> {};

template <typename SPSRetTagT, typename RetT> class ResultSerializer {
public:
  static WrapperFunctionResult serialize(RetT Result) {
    WrapperFunctionResult R;
    if (!SPSArgList<SPSRetTagT>::toWrapperFunctionResult(R, Result))
      return WrapperFunctionResult::createOutOfBandError(
          "Could not serialize return value from wrapper function");
    return R;
  }
};

template <typename SPSRetTagT> class ResultSerializer<SPSRetTagT, Error> {
public:
  static WrapperFunctionResult serialize(Error Err) {
    WrapperFunctionResult R;
    if (!SPSArgList<SPSRetTagT>::toWrapperFunctionResult(
            R, toSPSSerializable(std::move(Err))))
      return WrapperFunctionResult::createOutOfBandError(
          "Could not serialize return value from wrapper function");
    return R;
  }
};

template <typename SPSRetTagT, typename T>
class ResultSerializer<SPSRetTagT, Expected<T>> {
public:
  static WrapperFunctionResult serialize(Expected<T> E) {
    WrapperFunctionResult R;
    if (!SPSArgList<SPSRetTagT>::toWrapperFunctionResult(
            R, toSPSSerializable(std::move(E))))
      return WrapperFunctionResult::createOutOfBandError(
          "Could not serialize return value from wrapper function");
    return R;
  }
};

template <typename SPSRetTagT, typename RetT> class ResultDeserializer {
public:
  static void makeSafe(RetT &Result) {}

  static Error deserialize(RetT &Result, const char *ArgData, size_t ArgSize) {
    SPSInputBuffer IB(ArgData, ArgSize);
    if (!SPSArgList<SPSRetTagT>::deserialize(IB, Result))
      return make_error<StringError>(
          "Error deserializing return value from blob in call");
    return Error::success();
  }
};

template <> class ResultDeserializer<SPSError, Error> {
public:
  static void makeSafe(Error &Err) { cantFail(std::move(Err)); }

  static Error deserialize(Error &Err, const char *ArgData, size_t ArgSize) {
    SPSInputBuffer IB(ArgData, ArgSize);
    SPSSerializableError BSE;
    if (!SPSArgList<SPSError>::deserialize(IB, BSE))
      return make_error<StringError>(
          "Error deserializing return value from blob in call");
    Err = fromSPSSerializable(std::move(BSE));
    return Error::success();
  }
};

template <typename SPSTagT, typename T>
class ResultDeserializer<SPSExpected<SPSTagT>, Expected<T>> {
public:
  static void makeSafe(Expected<T> &E) { cantFail(E.takeError()); }

  static Error deserialize(Expected<T> &E, const char *ArgData,
                           size_t ArgSize) {
    SPSInputBuffer IB(ArgData, ArgSize);
    SPSSerializableExpected<T> BSE;
    if (!SPSArgList<SPSExpected<SPSTagT>>::deserialize(IB, BSE))
      return make_error<StringError>(
          "Error deserializing return value from blob in call");
    E = fromSPSSerializable(std::move(BSE));
    return Error::success();
  }
};

} // end namespace detail

template <typename SPSSignature> class WrapperFunction;

template <typename SPSRetTagT, typename... SPSTagTs>
class WrapperFunction<SPSRetTagT(SPSTagTs...)> {
private:
  template <typename RetT>
  using ResultSerializer = detail::ResultSerializer<SPSRetTagT, RetT>;

public:
  template <typename RetT, typename... ArgTs>
  static Error call(const void *FnTag, RetT &Result, const ArgTs &...Args) {

    // RetT might be an Error or Expected value. Set the checked flag now:
    // we don't want the user to have to check the unused result if this
    // operation fails.
    detail::ResultDeserializer<SPSRetTagT, RetT>::makeSafe(Result);

    if (ORC_RT_UNLIKELY(!&__orc_rt_jit_dispatch_ctx))
      return make_error<StringError>("__orc_jtjit_dispatch_ctx not set");
    if (ORC_RT_UNLIKELY(!&__orc_rt_jit_dispatch_ctx))
      return make_error<StringError>("__orc_jtjit_dispatch not set");

    WrapperFunctionResult ArgBuffer;
    if (!SPSArgList<SPSTagTs...>::toWrapperFunctionResult(ArgBuffer, Args...))
      return make_error<StringError>(
          "Error serializing arguments to blob in call");
    WrapperFunctionResult ResultBuffer = __orc_rt_jit_dispatch(
        &__orc_rt_jit_dispatch_ctx, FnTag, ArgBuffer.data(), ArgBuffer.size());
    if (auto ErrMsg = ResultBuffer.getOutOfBandError())
      return make_error<StringError>(ErrMsg);

    return detail::ResultDeserializer<SPSRetTagT, RetT>::deserialize(
        Result, ResultBuffer.data(), ResultBuffer.size());
  }

  template <typename HandlerT>
  static WrapperFunctionResult handle(const char *ArgData, size_t ArgSize,
                                      HandlerT &&Handler) {
    using WFHH =
        detail::WrapperFunctionHandlerHelper<HandlerT, ResultSerializer,
                                             SPSTagTs...>;
    return WFHH::apply(std::forward<HandlerT>(Handler), ArgData, ArgSize);
  }

private:
  template <typename T> static const T &makeSerializable(const T &Value) {
    return Value;
  }

  static detail::SPSSerializableError makeSerializable(Error Err) {
    return detail::toSPSSerializable(std::move(Err));
  }

  template <typename T>
  static detail::SPSSerializableExpected<T> makeSerializable(Expected<T> E) {
    return detail::toSPSSerializable(std::move(E));
  }
};

template <typename... SPSTagTs>
class WrapperFunction<void(SPSTagTs...)>
    : private WrapperFunction<SPSEmpty(SPSTagTs...)> {
public:
  template <typename... ArgTs>
  static Error call(const void *FnTag, const ArgTs &...Args) {
    SPSEmpty BE;
    return WrapperFunction<SPSEmpty(SPSTagTs...)>::call(FnTag, BE, Args...);
  }

  using WrapperFunction<SPSEmpty(SPSTagTs...)>::handle;
};

} // end namespace __orc_rt

#endif // ORC_RT_WRAPPER_FUNCTION_UTILS_H
