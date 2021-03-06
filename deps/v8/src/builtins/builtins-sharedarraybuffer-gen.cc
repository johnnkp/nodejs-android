// Copyright 2017 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/builtins/builtins-utils-gen.h"
#include "src/builtins/builtins.h"
#include "src/codegen/code-stub-assembler.h"
#include "src/objects/objects.h"

namespace v8 {
namespace internal {

using compiler::Node;

class SharedArrayBufferBuiltinsAssembler : public CodeStubAssembler {
 public:
  explicit SharedArrayBufferBuiltinsAssembler(
      compiler::CodeAssemblerState* state)
      : CodeStubAssembler(state) {}

 protected:
  using AssemblerFunction = Node* (CodeAssembler::*)(MachineType type,
                                                     Node* base, Node* offset,
                                                     Node* value,
                                                     Node* value_high);
  TNode<JSArrayBuffer> ValidateIntegerTypedArray(
      TNode<Object> maybe_array, TNode<Context> context,
      TNode<Int32T>* out_elements_kind, TNode<RawPtrT>* out_backing_store,
      Label* detached);

  TNode<UintPtrT> ValidateAtomicAccess(TNode<JSTypedArray> array,
                                       TNode<Object> index,
                                       TNode<Context> context);

  inline void DebugSanityCheckAtomicIndex(TNode<JSTypedArray> array,
                                          TNode<UintPtrT> index);

  void AtomicBinopBuiltinCommon(TNode<Object> maybe_array, TNode<Object> index,
                                TNode<Object> value, TNode<Context> context,
                                AssemblerFunction function,
                                Runtime::FunctionId runtime_function,
                                const char* method_name);

  // Create a BigInt from the result of a 64-bit atomic operation, using
  // projections on 32-bit platforms.
  TNode<BigInt> BigIntFromSigned64(Node* signed64);
  TNode<BigInt> BigIntFromUnsigned64(Node* unsigned64);
};

// https://tc39.es/ecma262/#sec-validateintegertypedarray
TNode<JSArrayBuffer>
SharedArrayBufferBuiltinsAssembler::ValidateIntegerTypedArray(
    TNode<Object> maybe_array, TNode<Context> context,
    TNode<Int32T>* out_elements_kind, TNode<RawPtrT>* out_backing_store,
    Label* detached) {
  Label not_float_or_clamped(this), invalid(this);

  // The logic of TypedArrayBuiltinsAssembler::ValidateTypedArrayBuffer is
  // inlined to avoid duplicate error branches.

  // Fail if it is not a heap object.
  GotoIf(TaggedIsSmi(maybe_array), &invalid);

  // Fail if the array's instance type is not JSTypedArray.
  TNode<Map> map = LoadMap(CAST(maybe_array));
  GotoIfNot(IsJSTypedArrayMap(map), &invalid);
  TNode<JSTypedArray> array = CAST(maybe_array);

  // Fail if the array's JSArrayBuffer is detached.
  TNode<JSArrayBuffer> array_buffer = GetTypedArrayBuffer(context, array);
  GotoIf(IsDetachedBuffer(array_buffer), detached);

  // Fail if the array's element type is float32, float64 or clamped.
  STATIC_ASSERT(INT8_ELEMENTS < FLOAT32_ELEMENTS);
  STATIC_ASSERT(INT16_ELEMENTS < FLOAT32_ELEMENTS);
  STATIC_ASSERT(INT32_ELEMENTS < FLOAT32_ELEMENTS);
  STATIC_ASSERT(UINT8_ELEMENTS < FLOAT32_ELEMENTS);
  STATIC_ASSERT(UINT16_ELEMENTS < FLOAT32_ELEMENTS);
  STATIC_ASSERT(UINT32_ELEMENTS < FLOAT32_ELEMENTS);
  TNode<Int32T> elements_kind = LoadMapElementsKind(map);
  GotoIf(Int32LessThan(elements_kind, Int32Constant(FLOAT32_ELEMENTS)),
         &not_float_or_clamped);
  STATIC_ASSERT(BIGINT64_ELEMENTS > UINT8_CLAMPED_ELEMENTS);
  STATIC_ASSERT(BIGUINT64_ELEMENTS > UINT8_CLAMPED_ELEMENTS);
  Branch(Int32GreaterThan(elements_kind, Int32Constant(UINT8_CLAMPED_ELEMENTS)),
         &not_float_or_clamped, &invalid);

  BIND(&invalid);
  {
    ThrowTypeError(context, MessageTemplate::kNotIntegerTypedArray,
                   maybe_array);
  }

  BIND(&not_float_or_clamped);
  *out_elements_kind = elements_kind;

  TNode<RawPtrT> backing_store = LoadJSArrayBufferBackingStorePtr(array_buffer);
  TNode<UintPtrT> byte_offset = LoadJSArrayBufferViewByteOffset(array);
  *out_backing_store = RawPtrAdd(backing_store, Signed(byte_offset));

  return array_buffer;
}

// https://tc39.github.io/ecma262/#sec-validateatomicaccess
// ValidateAtomicAccess( typedArray, requestIndex )
TNode<UintPtrT> SharedArrayBufferBuiltinsAssembler::ValidateAtomicAccess(
    TNode<JSTypedArray> array, TNode<Object> index, TNode<Context> context) {
  Label done(this), range_error(this);

  // 1. Assert: typedArray is an Object that has a [[ViewedArrayBuffer]]
  // internal slot.
  // 2. Let length be typedArray.[[ArrayLength]].
  TNode<UintPtrT> array_length = LoadJSTypedArrayLength(array);

  // 3. Let accessIndex be ? ToIndex(requestIndex).
  TNode<UintPtrT> index_uintptr = ToIndex(context, index, &range_error);

  // 4. Assert: accessIndex ??? 0.
  // 5. If accessIndex ??? length, throw a RangeError exception.
  Branch(UintPtrLessThan(index_uintptr, array_length), &done, &range_error);

  BIND(&range_error);
  ThrowRangeError(context, MessageTemplate::kInvalidAtomicAccessIndex);

  // 6. Return accessIndex.
  BIND(&done);
  return index_uintptr;
}

void SharedArrayBufferBuiltinsAssembler::DebugSanityCheckAtomicIndex(
    TNode<JSTypedArray> array, TNode<UintPtrT> index) {
  // In Debug mode, we re-validate the index as a sanity check because ToInteger
  // above calls out to JavaScript. Atomics work on ArrayBuffers, which may be
  // detached, and detachment state must be checked and throw before this
  // check. The length cannot change.
  //
  // This function must always be called after ValidateIntegerTypedArray, which
  // will ensure that LoadJSArrayBufferViewBuffer will not be null.
  CSA_ASSERT(this, Word32BinaryNot(
                       IsDetachedBuffer(LoadJSArrayBufferViewBuffer(array))));
  CSA_ASSERT(this, UintPtrLessThan(index, LoadJSTypedArrayLength(array)));
}

TNode<BigInt> SharedArrayBufferBuiltinsAssembler::BigIntFromSigned64(
    Node* signed64) {
  if (Is64()) {
    return BigIntFromInt64(UncheckedCast<IntPtrT>(signed64));
  } else {
    TNode<IntPtrT> low = UncheckedCast<IntPtrT>(Projection(0, signed64));
    TNode<IntPtrT> high = UncheckedCast<IntPtrT>(Projection(1, signed64));
    return BigIntFromInt32Pair(low, high);
  }
}

TNode<BigInt> SharedArrayBufferBuiltinsAssembler::BigIntFromUnsigned64(
    Node* unsigned64) {
  if (Is64()) {
    return BigIntFromUint64(UncheckedCast<UintPtrT>(unsigned64));
  } else {
    TNode<UintPtrT> low = UncheckedCast<UintPtrT>(Projection(0, unsigned64));
    TNode<UintPtrT> high = UncheckedCast<UintPtrT>(Projection(1, unsigned64));
    return BigIntFromUint32Pair(low, high);
  }
}

// https://tc39.es/ecma262/#sec-atomicload
TF_BUILTIN(AtomicsLoad, SharedArrayBufferBuiltinsAssembler) {
  TNode<Object> maybe_array = CAST(Parameter(Descriptor::kArray));
  TNode<Object> index = CAST(Parameter(Descriptor::kIndex));
  TNode<Context> context = CAST(Parameter(Descriptor::kContext));

  // 1. Let buffer be ? ValidateIntegerTypedArray(typedArray).
  Label detached(this);
  TNode<Int32T> elements_kind;
  TNode<RawPtrT> backing_store;
  TNode<JSArrayBuffer> array_buffer = ValidateIntegerTypedArray(
      maybe_array, context, &elements_kind, &backing_store, &detached);
  TNode<JSTypedArray> array = CAST(maybe_array);

  // 2. Let i be ? ValidateAtomicAccess(typedArray, index).
  TNode<UintPtrT> index_word = ValidateAtomicAccess(array, index, context);

  // 3. If IsDetachedBuffer(buffer) is true, throw a TypeError exception.
  // 4. NOTE: The above check is not redundant with the check in
  // ValidateIntegerTypedArray because the call to ValidateAtomicAccess on the
  // preceding line can have arbitrary side effects, which could cause the
  // buffer to become detached.
  GotoIf(IsDetachedBuffer(array_buffer), &detached);

  // Steps 5-10.
  //
  // (Not copied from ecma262 due to the axiomatic nature of the memory model.)
  Label i8(this), u8(this), i16(this), u16(this), i32(this), u32(this),
      i64(this), u64(this), other(this);
  int32_t case_values[] = {
      INT8_ELEMENTS,  UINT8_ELEMENTS,  INT16_ELEMENTS,    UINT16_ELEMENTS,
      INT32_ELEMENTS, UINT32_ELEMENTS, BIGINT64_ELEMENTS, BIGUINT64_ELEMENTS,
  };
  Label* case_labels[] = {&i8, &u8, &i16, &u16, &i32, &u32, &i64, &u64};
  Switch(elements_kind, &other, case_values, case_labels,
         arraysize(case_labels));

  BIND(&i8);
  Return(
      SmiFromInt32(AtomicLoad(MachineType::Int8(), backing_store, index_word)));

  BIND(&u8);
  Return(SmiFromInt32(
      AtomicLoad(MachineType::Uint8(), backing_store, index_word)));

  BIND(&i16);
  Return(SmiFromInt32(
      AtomicLoad(MachineType::Int16(), backing_store, WordShl(index_word, 1))));

  BIND(&u16);
  Return(SmiFromInt32(AtomicLoad(MachineType::Uint16(), backing_store,
                                 WordShl(index_word, 1))));

  BIND(&i32);
  Return(ChangeInt32ToTagged(
      AtomicLoad(MachineType::Int32(), backing_store, WordShl(index_word, 2))));

  BIND(&u32);
  Return(ChangeUint32ToTagged(AtomicLoad(MachineType::Uint32(), backing_store,
                                         WordShl(index_word, 2))));
#if V8_TARGET_ARCH_MIPS && !_MIPS_ARCH_MIPS32R6
  BIND(&i64);
  Goto(&u64);

  BIND(&u64);
  {
    TNode<Number> index_number = ChangeUintPtrToTagged(index_word);
    Return(CallRuntime(Runtime::kAtomicsLoad64, context, array, index_number));
  }
#else
  BIND(&i64);
  // This uses Uint64() intentionally: AtomicLoad is not implemented for
  // Int64(), which is fine because the machine instruction only cares
  // about words.
  Return(BigIntFromSigned64(AtomicLoad(MachineType::Uint64(), backing_store,
                                       WordShl(index_word, 3))));

  BIND(&u64);
  Return(BigIntFromUnsigned64(AtomicLoad(MachineType::Uint64(), backing_store,
                                         WordShl(index_word, 3))));
#endif

  // This shouldn't happen, we've already validated the type.
  BIND(&other);
  Unreachable();

  BIND(&detached);
  {
    ThrowTypeError(context, MessageTemplate::kDetachedOperation,
                   "Atomics.load");
  }
}

// https://tc39.es/ecma262/#sec-atomics.store
TF_BUILTIN(AtomicsStore, SharedArrayBufferBuiltinsAssembler) {
  TNode<Object> maybe_array = CAST(Parameter(Descriptor::kArray));
  TNode<Object> index = CAST(Parameter(Descriptor::kIndex));
  TNode<Object> value = CAST(Parameter(Descriptor::kValue));
  TNode<Context> context = CAST(Parameter(Descriptor::kContext));

  // 1. Let buffer be ? ValidateIntegerTypedArray(typedArray).
  Label detached(this);
  TNode<Int32T> elements_kind;
  TNode<RawPtrT> backing_store;
  TNode<JSArrayBuffer> array_buffer = ValidateIntegerTypedArray(
      maybe_array, context, &elements_kind, &backing_store, &detached);
  TNode<JSTypedArray> array = CAST(maybe_array);

  // 2. Let i be ? ValidateAtomicAccess(typedArray, index).
  TNode<UintPtrT> index_word = ValidateAtomicAccess(array, index, context);

  Label u8(this), u16(this), u32(this), u64(this), other(this);

  // 3. Let arrayTypeName be typedArray.[[TypedArrayName]].
  // 4. If arrayTypeName is "BigUint64Array" or "BigInt64Array",
  //    let v be ? ToBigInt(value).
  STATIC_ASSERT(BIGINT64_ELEMENTS > INT32_ELEMENTS);
  STATIC_ASSERT(BIGUINT64_ELEMENTS > INT32_ELEMENTS);
  GotoIf(Int32GreaterThan(elements_kind, Int32Constant(INT32_ELEMENTS)), &u64);

  // 5. Otherwise, let v be ? ToInteger(value).
  TNode<Number> value_integer = ToInteger_Inline(context, value);

  // 6. If IsDetachedBuffer(buffer) is true, throw a TypeError exception.
  // 7. NOTE: The above check is not redundant with the check in
  // ValidateIntegerTypedArray because the call to ToBigInt or ToInteger on the
  // preceding lines can have arbitrary side effects, which could cause the
  // buffer to become detached.
  GotoIf(IsDetachedBuffer(array_buffer), &detached);

  TNode<Word32T> value_word32 = TruncateTaggedToWord32(context, value_integer);

  DebugSanityCheckAtomicIndex(array, index_word);

  // Steps 8-13.
  //
  // (Not copied from ecma262 due to the axiomatic nature of the memory model.)
  int32_t case_values[] = {
      INT8_ELEMENTS,   UINT8_ELEMENTS, INT16_ELEMENTS,
      UINT16_ELEMENTS, INT32_ELEMENTS, UINT32_ELEMENTS,
  };
  Label* case_labels[] = {&u8, &u8, &u16, &u16, &u32, &u32};
  Switch(elements_kind, &other, case_values, case_labels,
         arraysize(case_labels));

  BIND(&u8);
  AtomicStore(MachineRepresentation::kWord8, backing_store, index_word,
              value_word32);
  Return(value_integer);

  BIND(&u16);
  AtomicStore(MachineRepresentation::kWord16, backing_store,
              WordShl(index_word, 1), value_word32);
  Return(value_integer);

  BIND(&u32);
  AtomicStore(MachineRepresentation::kWord32, backing_store,
              WordShl(index_word, 2), value_word32);
  Return(value_integer);

  BIND(&u64);
#if V8_TARGET_ARCH_MIPS && !_MIPS_ARCH_MIPS32R6
  TNode<Number> index_number = ChangeUintPtrToTagged(index_word);
  Return(CallRuntime(Runtime::kAtomicsStore64, context, array, index_number,
                     value));
#else
  // 4. If arrayTypeName is "BigUint64Array" or "BigInt64Array",
  //    let v be ? ToBigInt(value).
  TNode<BigInt> value_bigint = ToBigInt(context, value);

  // 6. If IsDetachedBuffer(buffer) is true, throw a TypeError exception.
  GotoIf(IsDetachedBuffer(array_buffer), &detached);

  DebugSanityCheckAtomicIndex(array, index_word);

  TVARIABLE(UintPtrT, var_low);
  TVARIABLE(UintPtrT, var_high);
  BigIntToRawBytes(value_bigint, &var_low, &var_high);
  TNode<UintPtrT> high = Is64() ? TNode<UintPtrT>() : var_high.value();
  AtomicStore(MachineRepresentation::kWord64, backing_store,
              WordShl(index_word, 3), var_low.value(), high);
  Return(value_bigint);
#endif

  // This shouldn't happen, we've already validated the type.
  BIND(&other);
  Unreachable();

  BIND(&detached);
  {
    ThrowTypeError(context, MessageTemplate::kDetachedOperation,
                   "Atomics.store");
  }
}

// https://tc39.es/ecma262/#sec-atomics.exchange
TF_BUILTIN(AtomicsExchange, SharedArrayBufferBuiltinsAssembler) {
  TNode<Object> maybe_array = CAST(Parameter(Descriptor::kArray));
  TNode<Object> index = CAST(Parameter(Descriptor::kIndex));
  TNode<Object> value = CAST(Parameter(Descriptor::kValue));
  TNode<Context> context = CAST(Parameter(Descriptor::kContext));

  // Inlines AtomicReadModifyWrite
  // https://tc39.es/ecma262/#sec-atomicreadmodifywrite

  // 1. Let buffer be ? ValidateIntegerTypedArray(typedArray).
  Label detached(this);
  TNode<Int32T> elements_kind;
  TNode<RawPtrT> backing_store;
  TNode<JSArrayBuffer> array_buffer = ValidateIntegerTypedArray(
      maybe_array, context, &elements_kind, &backing_store, &detached);
  TNode<JSTypedArray> array = CAST(maybe_array);

  // 2. Let i be ? ValidateAtomicAccess(typedArray, index).
  TNode<UintPtrT> index_word = ValidateAtomicAccess(array, index, context);

#if V8_TARGET_ARCH_MIPS || V8_TARGET_ARCH_MIPS64
  USE(array_buffer);
  TNode<Number> index_number = ChangeUintPtrToTagged(index_word);
  Return(CallRuntime(Runtime::kAtomicsExchange, context, array, index_number,
                     value));
#else

  Label i8(this), u8(this), i16(this), u16(this), i32(this), u32(this),
      i64(this), u64(this), big(this), other(this);

  // 3. Let arrayTypeName be typedArray.[[TypedArrayName]].
  // 4. If typedArray.[[ContentType]] is BigInt, let v be ? ToBigInt(value).
  STATIC_ASSERT(BIGINT64_ELEMENTS > INT32_ELEMENTS);
  STATIC_ASSERT(BIGUINT64_ELEMENTS > INT32_ELEMENTS);
  GotoIf(Int32GreaterThan(elements_kind, Int32Constant(INT32_ELEMENTS)), &big);

  // 5. Otherwise, let v be ? ToInteger(value).
  TNode<Number> value_integer = ToInteger_Inline(context, value);

  // 6. If IsDetachedBuffer(buffer) is true, throw a TypeError exception.
  // 7. NOTE: The above check is not redundant with the check in
  // ValidateIntegerTypedArray because the call to ToBigInt or ToInteger on the
  // preceding lines can have arbitrary side effects, which could cause the
  // buffer to become detached.
  GotoIf(IsDetachedBuffer(array_buffer), &detached);

  DebugSanityCheckAtomicIndex(array, index_word);

  TNode<Word32T> value_word32 = TruncateTaggedToWord32(context, value_integer);

  // Steps 8-12.
  //
  // (Not copied from ecma262 due to the axiomatic nature of the memory model.)
  int32_t case_values[] = {
      INT8_ELEMENTS,   UINT8_ELEMENTS, INT16_ELEMENTS,
      UINT16_ELEMENTS, INT32_ELEMENTS, UINT32_ELEMENTS,
  };
  Label* case_labels[] = {
      &i8, &u8, &i16, &u16, &i32, &u32,
  };
  Switch(elements_kind, &other, case_values, case_labels,
         arraysize(case_labels));

  BIND(&i8);
  Return(SmiFromInt32(AtomicExchange(MachineType::Int8(), backing_store,
                                     index_word, value_word32)));

  BIND(&u8);
  Return(SmiFromInt32(AtomicExchange(MachineType::Uint8(), backing_store,
                                     index_word, value_word32)));

  BIND(&i16);
  Return(SmiFromInt32(AtomicExchange(MachineType::Int16(), backing_store,
                                     WordShl(index_word, 1), value_word32)));

  BIND(&u16);
  Return(SmiFromInt32(AtomicExchange(MachineType::Uint16(), backing_store,
                                     WordShl(index_word, 1), value_word32)));

  BIND(&i32);
  Return(ChangeInt32ToTagged(AtomicExchange(MachineType::Int32(), backing_store,
                                            WordShl(index_word, 2),
                                            value_word32)));

  BIND(&u32);
  Return(ChangeUint32ToTagged(
      AtomicExchange(MachineType::Uint32(), backing_store,
                     WordShl(index_word, 2), value_word32)));

  BIND(&big);
  // 4. If typedArray.[[ContentType]] is BigInt, let v be ? ToBigInt(value).
  TNode<BigInt> value_bigint = ToBigInt(context, value);

  // 6. If IsDetachedBuffer(buffer) is true, throw a TypeError exception.
  GotoIf(IsDetachedBuffer(array_buffer), &detached);

  DebugSanityCheckAtomicIndex(array, index_word);

  TVARIABLE(UintPtrT, var_low);
  TVARIABLE(UintPtrT, var_high);
  BigIntToRawBytes(value_bigint, &var_low, &var_high);
  TNode<UintPtrT> high = Is64() ? TNode<UintPtrT>() : var_high.value();
  GotoIf(Word32Equal(elements_kind, Int32Constant(BIGINT64_ELEMENTS)), &i64);
  GotoIf(Word32Equal(elements_kind, Int32Constant(BIGUINT64_ELEMENTS)), &u64);
  Unreachable();

  BIND(&i64);
  // This uses Uint64() intentionally: AtomicExchange is not implemented for
  // Int64(), which is fine because the machine instruction only cares
  // about words.
  Return(BigIntFromSigned64(AtomicExchange(MachineType::Uint64(), backing_store,
                                           WordShl(index_word, 3),
                                           var_low.value(), high)));

  BIND(&u64);
  Return(BigIntFromUnsigned64(
      AtomicExchange(MachineType::Uint64(), backing_store,
                     WordShl(index_word, 3), var_low.value(), high)));

  // This shouldn't happen, we've already validated the type.
  BIND(&other);
  Unreachable();
#endif  // V8_TARGET_ARCH_MIPS || V8_TARGET_ARCH_MIPS64

  BIND(&detached);
  {
    ThrowTypeError(context, MessageTemplate::kDetachedOperation,
                   "Atomics.exchange");
  }
}

// https://tc39.es/ecma262/#sec-atomics.compareexchange
TF_BUILTIN(AtomicsCompareExchange, SharedArrayBufferBuiltinsAssembler) {
  TNode<Object> maybe_array = CAST(Parameter(Descriptor::kArray));
  TNode<Object> index = CAST(Parameter(Descriptor::kIndex));
  TNode<Object> old_value = CAST(Parameter(Descriptor::kOldValue));
  TNode<Object> new_value = CAST(Parameter(Descriptor::kNewValue));
  TNode<Context> context = CAST(Parameter(Descriptor::kContext));

  // 1. Let buffer be ? ValidateIntegerTypedArray(typedArray).
  Label detached(this);
  TNode<Int32T> elements_kind;
  TNode<RawPtrT> backing_store;
  TNode<JSArrayBuffer> array_buffer = ValidateIntegerTypedArray(
      maybe_array, context, &elements_kind, &backing_store, &detached);
  TNode<JSTypedArray> array = CAST(maybe_array);

  // 2. Let i be ? ValidateAtomicAccess(typedArray, index).
  TNode<UintPtrT> index_word = ValidateAtomicAccess(array, index, context);

#if V8_TARGET_ARCH_MIPS || V8_TARGET_ARCH_MIPS64 || V8_TARGET_ARCH_PPC64 || \
    V8_TARGET_ARCH_PPC || V8_TARGET_ARCH_S390 || V8_TARGET_ARCH_S390X
  USE(array_buffer);
  TNode<Number> index_number = ChangeUintPtrToTagged(index_word);
  Return(CallRuntime(Runtime::kAtomicsCompareExchange, context, array,
                     index_number, old_value, new_value));
#else
  Label i8(this), u8(this), i16(this), u16(this), i32(this), u32(this),
      i64(this), u64(this), big(this), other(this);

  // 3. Let arrayTypeName be typedArray.[[TypedArrayName]].
  // 4. If typedArray.[[ContentType]] is BigInt, then
  //   a. Let expected be ? ToBigInt(expectedValue).
  //   b. Let replacement be ? ToBigInt(replacementValue).
  STATIC_ASSERT(BIGINT64_ELEMENTS > INT32_ELEMENTS);
  STATIC_ASSERT(BIGUINT64_ELEMENTS > INT32_ELEMENTS);
  GotoIf(Int32GreaterThan(elements_kind, Int32Constant(INT32_ELEMENTS)), &big);

  // 5. Else,
  //   a. Let expected be ? ToInteger(expectedValue).
  //   b. Let replacement be ? ToInteger(replacementValue).
  TNode<Number> old_value_integer = ToInteger_Inline(context, old_value);
  TNode<Number> new_value_integer = ToInteger_Inline(context, new_value);

  // 6. If IsDetachedBuffer(buffer) is true, throw a TypeError exception.
  // 7. NOTE: The above check is not redundant with the check in
  // ValidateIntegerTypedArray because the call to ToBigInt or ToInteger on the
  // preceding lines can have arbitrary side effects, which could cause the
  // buffer to become detached.
  GotoIf(IsDetachedBuffer(array_buffer), &detached);

  DebugSanityCheckAtomicIndex(array, index_word);

  TNode<Word32T> old_value_word32 =
      TruncateTaggedToWord32(context, old_value_integer);
  TNode<Word32T> new_value_word32 =
      TruncateTaggedToWord32(context, new_value_integer);

  // Steps 8-14.
  //
  // (Not copied from ecma262 due to the axiomatic nature of the memory model.)
  int32_t case_values[] = {
      INT8_ELEMENTS,   UINT8_ELEMENTS, INT16_ELEMENTS,
      UINT16_ELEMENTS, INT32_ELEMENTS, UINT32_ELEMENTS,
  };
  Label* case_labels[] = {
      &i8, &u8, &i16, &u16, &i32, &u32,
  };
  Switch(elements_kind, &other, case_values, case_labels,
         arraysize(case_labels));

  BIND(&i8);
  Return(SmiFromInt32(AtomicCompareExchange(MachineType::Int8(), backing_store,
                                            index_word, old_value_word32,
                                            new_value_word32)));

  BIND(&u8);
  Return(SmiFromInt32(AtomicCompareExchange(MachineType::Uint8(), backing_store,
                                            index_word, old_value_word32,
                                            new_value_word32)));

  BIND(&i16);
  Return(SmiFromInt32(AtomicCompareExchange(
      MachineType::Int16(), backing_store, WordShl(index_word, 1),
      old_value_word32, new_value_word32)));

  BIND(&u16);
  Return(SmiFromInt32(AtomicCompareExchange(
      MachineType::Uint16(), backing_store, WordShl(index_word, 1),
      old_value_word32, new_value_word32)));

  BIND(&i32);
  Return(ChangeInt32ToTagged(AtomicCompareExchange(
      MachineType::Int32(), backing_store, WordShl(index_word, 2),
      old_value_word32, new_value_word32)));

  BIND(&u32);
  Return(ChangeUint32ToTagged(AtomicCompareExchange(
      MachineType::Uint32(), backing_store, WordShl(index_word, 2),
      old_value_word32, new_value_word32)));

  BIND(&big);
  // 4. If typedArray.[[ContentType]] is BigInt, then
  //   a. Let expected be ? ToBigInt(expectedValue).
  //   b. Let replacement be ? ToBigInt(replacementValue).
  TNode<BigInt> old_value_bigint = ToBigInt(context, old_value);
  TNode<BigInt> new_value_bigint = ToBigInt(context, new_value);

  // 6. If IsDetachedBuffer(buffer) is true, throw a TypeError exception.
  GotoIf(IsDetachedBuffer(array_buffer), &detached);

  DebugSanityCheckAtomicIndex(array, index_word);

  TVARIABLE(UintPtrT, var_old_low);
  TVARIABLE(UintPtrT, var_old_high);
  TVARIABLE(UintPtrT, var_new_low);
  TVARIABLE(UintPtrT, var_new_high);
  BigIntToRawBytes(old_value_bigint, &var_old_low, &var_old_high);
  BigIntToRawBytes(new_value_bigint, &var_new_low, &var_new_high);
  TNode<UintPtrT> old_high = Is64() ? TNode<UintPtrT>() : var_old_high.value();
  TNode<UintPtrT> new_high = Is64() ? TNode<UintPtrT>() : var_new_high.value();
  GotoIf(Word32Equal(elements_kind, Int32Constant(BIGINT64_ELEMENTS)), &i64);
  GotoIf(Word32Equal(elements_kind, Int32Constant(BIGUINT64_ELEMENTS)), &u64);
  Unreachable();

  BIND(&i64);
  // This uses Uint64() intentionally: AtomicCompareExchange is not implemented
  // for Int64(), which is fine because the machine instruction only cares
  // about words.
  Return(BigIntFromSigned64(AtomicCompareExchange(
      MachineType::Uint64(), backing_store, WordShl(index_word, 3),
      var_old_low.value(), var_new_low.value(), old_high, new_high)));

  BIND(&u64);
  Return(BigIntFromUnsigned64(AtomicCompareExchange(
      MachineType::Uint64(), backing_store, WordShl(index_word, 3),
      var_old_low.value(), var_new_low.value(), old_high, new_high)));

  // This shouldn't happen, we've already validated the type.
  BIND(&other);
  Unreachable();
#endif  // V8_TARGET_ARCH_MIPS || V8_TARGET_ARCH_MIPS64 || V8_TARGET_ARCH_PPC64
        // || V8_TARGET_ARCH_PPC || V8_TARGET_ARCH_S390 || V8_TARGET_ARCH_S390X

  BIND(&detached);
  {
    ThrowTypeError(context, MessageTemplate::kDetachedOperation,
                   "Atomics.store");
  }
}

#define BINOP_BUILTIN(op, method_name)                              \
  TF_BUILTIN(Atomics##op, SharedArrayBufferBuiltinsAssembler) {     \
    TNode<Object> array = CAST(Parameter(Descriptor::kArray));      \
    TNode<Object> index = CAST(Parameter(Descriptor::kIndex));      \
    TNode<Object> value = CAST(Parameter(Descriptor::kValue));      \
    TNode<Context> context = CAST(Parameter(Descriptor::kContext)); \
    AtomicBinopBuiltinCommon(array, index, value, context,          \
                             &CodeAssembler::Atomic##op,            \
                             Runtime::kAtomics##op, method_name);   \
  }
// https://tc39.es/ecma262/#sec-atomics.add
BINOP_BUILTIN(Add, "Atomics.add")
// https://tc39.es/ecma262/#sec-atomics.sub
BINOP_BUILTIN(Sub, "Atomics.sub")
// https://tc39.es/ecma262/#sec-atomics.and
BINOP_BUILTIN(And, "Atomics.and")
// https://tc39.es/ecma262/#sec-atomics.or
BINOP_BUILTIN(Or, "Atomics.or")
// https://tc39.es/ecma262/#sec-atomics.xor
BINOP_BUILTIN(Xor, "Atomics.xor")
#undef BINOP_BUILTIN

// https://tc39.es/ecma262/#sec-atomicreadmodifywrite
void SharedArrayBufferBuiltinsAssembler::AtomicBinopBuiltinCommon(
    TNode<Object> maybe_array, TNode<Object> index, TNode<Object> value,
    TNode<Context> context, AssemblerFunction function,
    Runtime::FunctionId runtime_function, const char* method_name) {
  // 1. Let buffer be ? ValidateIntegerTypedArray(typedArray).
  Label detached(this);
  TNode<Int32T> elements_kind;
  TNode<RawPtrT> backing_store;
  TNode<JSArrayBuffer> array_buffer = ValidateIntegerTypedArray(
      maybe_array, context, &elements_kind, &backing_store, &detached);
  TNode<JSTypedArray> array = CAST(maybe_array);

  // 2. Let i be ? ValidateAtomicAccess(typedArray, index).
  TNode<UintPtrT> index_word = ValidateAtomicAccess(array, index, context);

#if V8_TARGET_ARCH_MIPS || V8_TARGET_ARCH_MIPS64 || V8_TARGET_ARCH_PPC64 || \
    V8_TARGET_ARCH_PPC || V8_TARGET_ARCH_S390 || V8_TARGET_ARCH_S390X
  USE(array_buffer);
  TNode<Number> index_number = ChangeUintPtrToTagged(index_word);
  Return(CallRuntime(runtime_function, context, array, index_number, value));
#else
  Label i8(this), u8(this), i16(this), u16(this), i32(this), u32(this),
      i64(this), u64(this), big(this), other(this);

  // 3. Let arrayTypeName be typedArray.[[TypedArrayName]].
  // 4. If typedArray.[[ContentType]] is BigInt, let v be ? ToBigInt(value).
  STATIC_ASSERT(BIGINT64_ELEMENTS > INT32_ELEMENTS);
  STATIC_ASSERT(BIGUINT64_ELEMENTS > INT32_ELEMENTS);
  GotoIf(Int32GreaterThan(elements_kind, Int32Constant(INT32_ELEMENTS)), &big);

  // 5. Otherwise, let v be ? ToInteger(value).
  TNode<Number> value_integer = ToInteger_Inline(context, value);

  // 6. If IsDetachedBuffer(buffer) is true, throw a TypeError exception.
  // 7. NOTE: The above check is not redundant with the check in
  // ValidateIntegerTypedArray because the call to ToBigInt or ToInteger on the
  // preceding lines can have arbitrary side effects, which could cause the
  // buffer to become detached.
  GotoIf(IsDetachedBuffer(array_buffer), &detached);

  DebugSanityCheckAtomicIndex(array, index_word);

  TNode<Word32T> value_word32 = TruncateTaggedToWord32(context, value_integer);

  // Steps 8-12.
  //
  // (Not copied from ecma262 due to the axiomatic nature of the memory model.)
  int32_t case_values[] = {
      INT8_ELEMENTS,   UINT8_ELEMENTS, INT16_ELEMENTS,
      UINT16_ELEMENTS, INT32_ELEMENTS, UINT32_ELEMENTS,
  };
  Label* case_labels[] = {
      &i8, &u8, &i16, &u16, &i32, &u32,
  };
  Switch(elements_kind, &other, case_values, case_labels,
         arraysize(case_labels));

  BIND(&i8);
  Return(SmiFromInt32((this->*function)(MachineType::Int8(), backing_store,
                                        index_word, value_word32, nullptr)));

  BIND(&u8);
  Return(SmiFromInt32((this->*function)(MachineType::Uint8(), backing_store,
                                        index_word, value_word32, nullptr)));

  BIND(&i16);
  Return(SmiFromInt32((this->*function)(MachineType::Int16(), backing_store,
                                        WordShl(index_word, 1), value_word32,
                                        nullptr)));

  BIND(&u16);
  Return(SmiFromInt32((this->*function)(MachineType::Uint16(), backing_store,
                                        WordShl(index_word, 1), value_word32,
                                        nullptr)));

  BIND(&i32);
  Return(ChangeInt32ToTagged(
      (this->*function)(MachineType::Int32(), backing_store,
                        WordShl(index_word, 2), value_word32, nullptr)));

  BIND(&u32);
  Return(ChangeUint32ToTagged(
      (this->*function)(MachineType::Uint32(), backing_store,
                        WordShl(index_word, 2), value_word32, nullptr)));

  BIND(&big);
  // 4. If typedArray.[[ContentType]] is BigInt, let v be ? ToBigInt(value).
  TNode<BigInt> value_bigint = ToBigInt(context, value);

  // 6. If IsDetachedBuffer(buffer) is true, throw a TypeError exception.
  GotoIf(IsDetachedBuffer(array_buffer), &detached);

  DebugSanityCheckAtomicIndex(array, index_word);

  TVARIABLE(UintPtrT, var_low);
  TVARIABLE(UintPtrT, var_high);
  BigIntToRawBytes(value_bigint, &var_low, &var_high);
  TNode<UintPtrT> high = Is64() ? TNode<UintPtrT>() : var_high.value();
  GotoIf(Word32Equal(elements_kind, Int32Constant(BIGINT64_ELEMENTS)), &i64);
  GotoIf(Word32Equal(elements_kind, Int32Constant(BIGUINT64_ELEMENTS)), &u64);
  Unreachable();

  BIND(&i64);
  // This uses Uint64() intentionally: Atomic* ops are not implemented for
  // Int64(), which is fine because the machine instructions only care
  // about words.
  Return(BigIntFromSigned64(
      (this->*function)(MachineType::Uint64(), backing_store,
                        WordShl(index_word, 3), var_low.value(), high)));

  BIND(&u64);
  Return(BigIntFromUnsigned64(
      (this->*function)(MachineType::Uint64(), backing_store,
                        WordShl(index_word, 3), var_low.value(), high)));

  // This shouldn't happen, we've already validated the type.
  BIND(&other);
  Unreachable();
#endif  // V8_TARGET_ARCH_MIPS || V8_TARGET_ARCH_MIPS64 || V8_TARGET_ARCH_PPC64
        // || V8_TARGET_ARCH_PPC || V8_TARGET_ARCH_S390 || V8_TARGET_ARCH_S390X

  BIND(&detached);
  ThrowTypeError(context, MessageTemplate::kDetachedOperation, method_name);
}

}  // namespace internal
}  // namespace v8
