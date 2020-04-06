////////////////////////////////////////////////////////////////////////////////////////////////////
//
//  Project:  Embedded Learning Library (ELL)
//  File:     LLVMContext.cpp (value)
//  Authors:  Kern Handa
//
////////////////////////////////////////////////////////////////////////////////////////////////////

#include "LLVMContext.h"
#include "FunctionDeclaration.h"
#include "Scalar.h"
#include "Value.h"

#include <emitters/include/IRModuleEmitter.h>

#include <utilities/include/StringUtil.h>

#include <numeric>

#include <llvm/Support/raw_os_ostream.h>

using namespace std::string_literals;

namespace ell
{
namespace value
{
    using namespace detail;
    using namespace emitters;
    using namespace utilities;

    namespace
    {

        detail::ValueTypeDescription LLVMTypeToValueType(LLVMType type)
        {
            switch (type->getTypeID())
            {
            case llvm::Type::TypeID::FloatTyID:
                return { ValueType::Float, 0 };
            case llvm::Type::TypeID::DoubleTyID:
                return { ValueType::Double, 0 };
            case llvm::Type::TypeID::IntegerTyID:
                switch (type->getIntegerBitWidth())
                {
                case 1:
                    return { ValueType::Boolean, 0 };
                case 8:
                    return { ValueType::Char8, 0 };
                case 16:
                    return { ValueType::Int16, 0 };
                case 32:
                    return { ValueType::Int32, 0 };
                case 64:
                    return { ValueType::Int64, 0 };
                default:
                    break;
                }
            case llvm::Type::TypeID::PointerTyID:
            {
                auto elementType = type->getPointerElementType();

                auto underlyingType = LLVMTypeToValueType(elementType);
                underlyingType.second += 1;
                return underlyingType;
            }
            case llvm::Type::TypeID::ArrayTyID:
            {
                auto elementType = type->getArrayElementType();

                auto underlyingType = LLVMTypeToValueType(elementType);
                underlyingType.second += 1;
                return underlyingType;
            }
            default:
                break;
            }
            throw LogicException(LogicExceptionErrors::illegalState);
        }

        LLVMType ValueTypeToLLVMType(IREmitter& emitter, detail::ValueTypeDescription typeDescription)
        {
            auto& builder = emitter.GetIRBuilder();
            LLVMType type = nullptr;
            switch (typeDescription.first)
            {
            case ValueType::Boolean:
                if (typeDescription.second == 0)
                {
                    type = builder.getInt1Ty();
                }
                else
                {
                    type = builder.getInt8Ty();
                }
                break;
            case ValueType::Byte:
                [[fallthrough]];
            case ValueType::Char8:
                type = builder.getInt8Ty();
                break;
            case ValueType::Int16:
                type = builder.getInt16Ty();
                break;
            case ValueType::Int32:
                type = builder.getInt32Ty();
                break;
            case ValueType::Int64:
                type = builder.getInt64Ty();
                break;
            case ValueType::Float:
                type = builder.getFloatTy();
                break;
            case ValueType::Double:
                type = builder.getDoubleTy();
                break;
            case ValueType::Void:
                type = builder.getVoidTy();
                break;
            case ValueType::Undefined:
                [[fallthrough]];
            default:
                throw LogicException(LogicExceptionErrors::illegalState);
            }

            for (int ptrLevel = 0; ptrLevel < typeDescription.second; ++ptrLevel)
            {
                type = type->getPointerTo();
            }

            return type;
        }

        VariableType ValueTypeToVariableType(ValueType type)
        {
            // clang-format off

#define VALUE_TYPE_TO_VARIABLE_TYPE_MAPPING(x, y)  \
    case ValueType::x:                             \
        return VariableType::y

#define VALUE_TYPE_TO_VARIABLE_TYPE_PTR(x)         \
    case ValueType::x:                             \
        return VariableType::x##Pointer

            // clang-format on

            switch (type)
            {
                VALUE_TYPE_TO_VARIABLE_TYPE_MAPPING(Boolean, Boolean);
                VALUE_TYPE_TO_VARIABLE_TYPE_PTR(Byte);
                VALUE_TYPE_TO_VARIABLE_TYPE_PTR(Char8);
                VALUE_TYPE_TO_VARIABLE_TYPE_PTR(Int16);
                VALUE_TYPE_TO_VARIABLE_TYPE_PTR(Int32);
                VALUE_TYPE_TO_VARIABLE_TYPE_PTR(Int64);
                VALUE_TYPE_TO_VARIABLE_TYPE_PTR(Float);
                VALUE_TYPE_TO_VARIABLE_TYPE_PTR(Double);
                VALUE_TYPE_TO_VARIABLE_TYPE_MAPPING(Void, Void);
            default:
                throw LogicException(LogicExceptionErrors::illegalState);
            }

#undef VALUE_TYPE_TO_VARIABLE_TYPE_PTR

#undef VALUE_TYPE_TO_VARIABLE_TYPE_MAPPING
        }

        // TODO: Make this the basis of an iterator for MemoryLayout
        bool IncrementMemoryCoordinateImpl(int dimension, std::vector<int>& coordinate, const std::vector<int>& maxCoordinate)
        {
            // base case
            if (dimension < 0)
            {
                return false;
            }

            if (++coordinate[dimension] >= maxCoordinate[dimension])
            {
                coordinate[dimension] = 0;
                return IncrementMemoryCoordinateImpl(dimension - 1, coordinate, maxCoordinate);
            }

            return true;
        }

        bool IncrementMemoryCoordinate(std::vector<int>& coordinate, const std::vector<int>& maxCoordinate)
        {
            assert(coordinate.size() == maxCoordinate.size());
            return IncrementMemoryCoordinateImpl(static_cast<int>(maxCoordinate.size()) - 1, coordinate, maxCoordinate);
        }

        LLVMValue ToLLVMValue(Value value) { return value.Get<Emittable>().GetDataAs<LLVMValue>(); }

        auto SimpleNumericalFunctionIntrinsic(LLVMFunction (IRRuntime::*intrinsicFn)(VariableType)) -> std::function<Value(IRFunctionEmitter&, std::vector<Value>)>
        {
            return [intrinsicFn](IRFunctionEmitter& fnEmitter, std::vector<Value> args) -> Value {
                if (args.size() != 1)
                {
                    throw InputException(InputExceptionErrors::invalidSize);
                }

                const auto& value = args[0];
                if (value.GetBaseType() == ValueType::Boolean)
                {
                    throw InputException(InputExceptionErrors::typeMismatch);
                }

                auto variableType = [](ValueType type) {
                    switch (type)
                    {
                    case ValueType::Float:
                        return VariableType::Float;
                    default:
                        return VariableType::Double;
                    }
                }(value.GetBaseType());

                auto llvmFunc = std::invoke(intrinsicFn, fnEmitter.GetModule().GetRuntime(), variableType);

                Value returnValue = value::Allocate(value.GetBaseType(),
                                                    value.IsConstrained() ? value.GetLayout() : ScalarLayout);

                const auto& returnLayout = returnValue.GetLayout();
                auto maxCoordinate = returnLayout.GetActiveSize().ToVector();
                decltype(maxCoordinate) coordinate(maxCoordinate.size());
                auto inputLLVMValue = ToLLVMValue(value);
                auto returnLLVMValue = ToLLVMValue(returnValue);
                do
                {
                    auto logicalCoordinates = returnLayout.GetLogicalCoordinates(coordinate);
                    auto offset = static_cast<int>(returnLayout.GetLogicalEntryOffset(logicalCoordinates));

                    LLVMValue resultValue = nullptr;
                    if (value.IsFloatingPoint())
                    {
                        resultValue = fnEmitter.Call(llvmFunc, { fnEmitter.ValueAt(inputLLVMValue, offset) });
                    }
                    else
                    {
                        resultValue = fnEmitter.Call(llvmFunc, { fnEmitter.CastValue<double>(fnEmitter.ValueAt(inputLLVMValue, offset)) });
                    }
                    fnEmitter.SetValueAt(returnLLVMValue, offset, resultValue);
                } while (IncrementMemoryCoordinate(coordinate, maxCoordinate));

                return returnValue;
            };
        }

        auto PowFunctionIntrinsic() -> std::function<Value(IRFunctionEmitter&, std::vector<Value>)>
        {
            return [](IRFunctionEmitter& fnEmitter, std::vector<Value> args) -> Value {
                if (args.size() != 2)
                {
                    throw InputException(InputExceptionErrors::invalidSize);
                }

                const auto& value1 = args[0];
                const auto& value2 = args[1];
                if (value1.GetBaseType() != value2.GetBaseType())
                {
                    throw InputException(InputExceptionErrors::typeMismatch);
                }

                if (value1.GetBaseType() == ValueType::Boolean)
                {
                    throw InputException(InputExceptionErrors::typeMismatch);
                }

                if (value2.IsConstrained() && value2.GetLayout() != ScalarLayout)
                {
                    throw InputException(InputExceptionErrors::invalidSize);
                }

                auto variableType = [type = value1.GetBaseType()] {
                    switch (type)
                    {
                    case ValueType::Float:
                        return VariableType::Float;
                    default:
                        return VariableType::Double;
                    }
                }();

                auto llvmFunc = fnEmitter.GetModule().GetRuntime().GetPowFunction(variableType);

                Value returnValue = value::Allocate(value1.GetBaseType(),
                                                    value1.IsConstrained() ? value1.GetLayout() : ScalarLayout);

                const auto& returnLayout = returnValue.GetLayout();
                auto maxCoordinate = returnLayout.GetActiveSize().ToVector();
                decltype(maxCoordinate) coordinate(maxCoordinate.size());
                auto expLLVMValue = [&] {
                    if (value2.IsFloatingPoint())
                    {
                        return fnEmitter.ValueAt(ToLLVMValue(value2), 0);
                    }
                    else
                    {
                        return fnEmitter.CastValue<double>(fnEmitter.ValueAt(ToLLVMValue(value2), 0));
                    }
                }();
                auto baseLLVMValue = ToLLVMValue(value1);
                auto returnLLVMValue = ToLLVMValue(returnValue);
                do
                {
                    auto logicalCoordinates = returnLayout.GetLogicalCoordinates(coordinate);
                    auto offset = static_cast<int>(returnLayout.GetLogicalEntryOffset(logicalCoordinates));

                    LLVMValue resultValue = nullptr;
                    if (value1.IsFloatingPoint())
                    {
                        resultValue = fnEmitter.Call(llvmFunc, { fnEmitter.ValueAt(baseLLVMValue, offset), expLLVMValue });
                    }
                    else
                    {
                        resultValue = fnEmitter.Call(llvmFunc, { fnEmitter.CastValue<double>(fnEmitter.ValueAt(baseLLVMValue, offset)), expLLVMValue });
                    }
                    fnEmitter.SetValueAt(returnLLVMValue, offset, resultValue);

                } while (IncrementMemoryCoordinate(coordinate, maxCoordinate));

                return returnValue;
            };
        }

        auto CopySignFunctionIntrinsic() -> std::function<Value(IRFunctionEmitter&, std::vector<Value>)>
        {
            return [](IRFunctionEmitter& fnEmitter, std::vector<Value> args) -> Value {
                if (args.size() != 2)
                {
                    throw InputException(InputExceptionErrors::invalidSize);
                }

                const auto& value1 = args[0];
                const auto& value2 = args[1];
                if (value1.GetBaseType() != value2.GetBaseType())
                {
                    throw InputException(InputExceptionErrors::typeMismatch);
                }

                if (value1.GetBaseType() == ValueType::Boolean)
                {
                    throw InputException(InputExceptionErrors::typeMismatch);
                }

                if ((value1.IsConstrained() && value1.GetLayout() != ScalarLayout) ||
                    (value2.IsConstrained() && value2.GetLayout() != ScalarLayout))
                {
                    throw InputException(InputExceptionErrors::invalidSize);
                }

                Value result = value::Allocate(value1.GetBaseType(), ScalarLayout);

                auto llvmValue1 = fnEmitter.ValueAt(ToLLVMValue(value1), 0);
                auto llvmValue2 = fnEmitter.ValueAt(ToLLVMValue(value2), 0);
                auto variableType = [type = value1.GetBaseType()] {
                    switch (type)
                    {
                    case ValueType::Float:
                        return VariableType::Float;
                    default:
                        return VariableType::Double;
                    }
                }();

                auto llvmFunc = fnEmitter.GetModule().GetRuntime().GetCopySignFunction(variableType);
                auto resultValue = ToLLVMValue(result);
                fnEmitter.SetValueAt(resultValue, 0, fnEmitter.Call(llvmFunc, { llvmValue1, llvmValue2 }));
                return result;
            };
        }

        enum class MaxMinIntrinsic
        {
            Max,
            Min
        };

        auto MaxMinIntrinsicFunction(MaxMinIntrinsic intrinsic) -> std::function<Value(IRFunctionEmitter&, std::vector<Value>)>
        {
            return [intrinsic](IRFunctionEmitter& fnEmitter, std::vector<Value> args) -> Value {
                if (args.size() == 1)
                {
                    const auto& value = args[0];
                    if (value.GetBaseType() == ValueType::Boolean)
                    {
                        throw InputException(InputExceptionErrors::typeMismatch);
                    }

                    Value result = value::Allocate(value.GetBaseType(), ScalarLayout);
                    auto cmpOp = [&] {
                        switch (value.GetBaseType())
                        {
                        case ValueType::Float:
                            [[fallthrough]];
                        case ValueType::Double:
                            if (intrinsic == MaxMinIntrinsic::Max)
                            {
                                return TypedComparison::greaterThanOrEqualsFloat;
                            }
                            else
                            {
                                return TypedComparison::lessThanOrEqualsFloat;
                            }
                        default:
                            if (intrinsic == MaxMinIntrinsic::Max)
                            {
                                return TypedComparison::greaterThanOrEquals;
                            }
                            else
                            {
                                return TypedComparison::lessThanOrEquals;
                            }
                        }
                    }();
                    auto inputLLVMValue = ToLLVMValue(value);
                    auto resultLLVMValue = ToLLVMValue(result);

                    // set the initial value
                    fnEmitter.SetValueAt(resultLLVMValue, 0, fnEmitter.ValueAt(inputLLVMValue, 0));

                    const auto& inputLayout = value.GetLayout();
                    auto maxCoordinate = inputLayout.GetActiveSize().ToVector();
                    decltype(maxCoordinate) coordinate(maxCoordinate.size());

                    do
                    {
                        auto logicalCoordinates = inputLayout.GetLogicalCoordinates(coordinate);
                        auto offset = static_cast<int>(inputLayout.GetLogicalEntryOffset(logicalCoordinates));

                        auto op1 = fnEmitter.ValueAt(resultLLVMValue, 0);
                        auto op2 = fnEmitter.ValueAt(inputLLVMValue, offset);
                        auto cmp = fnEmitter.Comparison(cmpOp, op1, op2);
                        auto selected = fnEmitter.Select(cmp, op1, op2);
                        fnEmitter.SetValueAt(resultLLVMValue, 0, selected);
                    } while (IncrementMemoryCoordinate(coordinate, maxCoordinate));

                    return result;
                }
                else if (args.size() == 2)
                {
                    const auto& value1 = args[0];
                    const auto& value2 = args[1];
                    if (value1.GetBaseType() != value2.GetBaseType())
                    {
                        throw InputException(InputExceptionErrors::typeMismatch);
                    }

                    if (value1.GetBaseType() == ValueType::Boolean)
                    {
                        throw InputException(InputExceptionErrors::typeMismatch);
                    }

                    if ((value1.IsConstrained() && value1.GetLayout() != ScalarLayout) ||
                        (value2.IsConstrained() && value2.GetLayout() != ScalarLayout))
                    {
                        throw InputException(InputExceptionErrors::invalidSize);
                    }

                    Value result = value::Allocate(value1.GetBaseType(), ScalarLayout);
                    auto cmpOp = [&] {
                        switch (value1.GetBaseType())
                        {
                        case ValueType::Float:
                            [[fallthrough]];
                        case ValueType::Double:
                            if (intrinsic == MaxMinIntrinsic::Max)
                            {
                                return TypedComparison::greaterThanOrEqualsFloat;
                            }
                            else
                            {
                                return TypedComparison::lessThanOrEqualsFloat;
                            }
                        default:
                            if (intrinsic == MaxMinIntrinsic::Max)
                            {
                                return TypedComparison::greaterThanOrEquals;
                            }
                            else
                            {
                                return TypedComparison::lessThanOrEquals;
                            }
                        }
                    }();
                    auto llvmValue1 = fnEmitter.ValueAt(ToLLVMValue(value1), 0);
                    auto llvmValue2 = fnEmitter.ValueAt(ToLLVMValue(value2), 0);
                    auto cmp = fnEmitter.Comparison(cmpOp, llvmValue1, llvmValue2);

                    auto resultValue = ToLLVMValue(result);
                    fnEmitter.SetValueAt(resultValue, 0, fnEmitter.Select(cmp, llvmValue1, llvmValue2));
                    return result;
                }
                else
                {
                    throw InputException(InputExceptionErrors::invalidSize);
                }
            };
        }

        void ConstantForLoop(const MemoryLayout& layout, std::function<void(const MemoryCoordinates&)> fn)
        {
            auto maxCoordinate = layout.GetActiveSize().ToVector();
            decltype(maxCoordinate) coordinate(maxCoordinate.size());

            do
            {
                fn(layout.GetLogicalCoordinates(coordinate));
            } while (IncrementMemoryCoordinate(coordinate, maxCoordinate));
        }

        void ConstantForLoop(const MemoryLayout& layout, std::function<void(int)> fn)
        {
            ConstantForLoop(layout, [&](const MemoryCoordinates& coordinate) {
                fn(static_cast<int>(layout.GetLogicalEntryOffset(coordinate)));
            });
        }

    } // namespace

    struct LLVMContext::FunctionScope
    {
        FunctionScope(LLVMContext& context, IRFunctionEmitter& emitter) :
            context(context)
        {
            context._functionStack.push(emitter);
            context._promotedConstantStack.push({});
        }

        template <typename... Args>
        FunctionScope(LLVMContext& context, Args&&... args) :
            context(context)
        {
            context._functionStack.push(context._emitter.BeginFunction(std::forward<Args>(args)...));
            context._promotedConstantStack.push({});
        }

        ~FunctionScope()
        {
            context._functionStack.pop();
            context._promotedConstantStack.pop();
        }

        LLVMContext& context;
    };

    LLVMContext::LLVMContext(IRModuleEmitter& emitter) :
        _emitter(emitter),
        _computeContext(_emitter.GetModuleName())
    {
        _promotedConstantStack.push({});
    }

    IRModuleEmitter& LLVMContext::GetModuleEmitter() const { return _emitter; }

    Value LLVMContext::AllocateImpl(ValueType type, MemoryLayout layout)
    {
        auto& fn = GetFunctionEmitter();
        auto& irEmitter = fn.GetEmitter();

        auto llvmType = ValueTypeToLLVMType(irEmitter, { type, 0 });
        assert(!llvmType->isPointerTy());
        auto allocatedVariable = fn.Variable(llvmType, layout.GetMemorySize());
        fn.StoreZero(allocatedVariable, layout.GetMemorySize());

        return { Emittable{ allocatedVariable }, layout };
    }

    std::optional<Value> LLVMContext::GetGlobalValue(GlobalAllocationScope scope, std::string name)
    {
        std::string adjustedName = GetScopeAdjustedName(scope, name);
        if (auto it = _globals.find(adjustedName); it != _globals.end())
        {
            return Value(it->second.first, it->second.second);
        }

        return std::nullopt;
    }

    Value LLVMContext::GlobalAllocateImpl(GlobalAllocationScope scope, std::string name, ConstantData data, MemoryLayout layout)
    {
        std::string adjustedName = GetScopeAdjustedName(scope, name);

        if (_globals.find(adjustedName) != _globals.end())
        {
            throw InputException(InputExceptionErrors::invalidArgument,
                                 "Unexpected collision in global data allocation");
        }

        llvm::GlobalVariable* global = std::visit(
            [this, &adjustedName](auto&& vectorData) {
                using Type = std::decay_t<decltype(vectorData)>;

                if constexpr (std::is_same_v<Type, std::vector<utilities::Boolean>>)
                {
                    // IREmitter stores a vector of bool values as a bitvector, which
                    // breaks the memory model we need for our purposes.
                    // NB: This somewhat screws up our type system because we rely
                    // on LLVM to tell us the type, but here we set a different type
                    // altogether, with no discernable way of retrieving the fact that
                    // originally, this was a vector of bools. This will be rectified
                    // in the near future. (2018-11-08)
                    std::vector<char> transformedData(vectorData.begin(), vectorData.end());
                    return _emitter.GlobalArray(adjustedName, transformedData);
                }
                else
                {
                    return _emitter.GlobalArray(adjustedName, vectorData);
                }
            },
            data);
        auto dereferencedGlobal = _emitter.GetIREmitter().PointerOffset(global, _emitter.GetIREmitter().Literal(0));

        Emittable emittable{ dereferencedGlobal };
        _globals[adjustedName] = { emittable, layout };

        return Value(emittable, layout);
    }

    Value LLVMContext::GlobalAllocateImpl(GlobalAllocationScope scope, std::string name, ValueType type, MemoryLayout layout)
    {
        std::string adjustedName = GetScopeAdjustedName(scope, name);

        if (_globals.find(adjustedName) != _globals.end())
        {
            throw InputException(InputExceptionErrors::invalidArgument,
                                 FormatString("Global variable %s is already defined", adjustedName.c_str()));
        }

        auto global = _emitter.GlobalArray(adjustedName,
                                           ValueTypeToLLVMType(_emitter.GetIREmitter(), { type, 0 }),
                                           layout.GetMemorySize());

        auto dereferencedGlobal = _emitter.GetIREmitter().PointerOffset(global, _emitter.GetIREmitter().Literal(0));

        Emittable emittable{ dereferencedGlobal };
        _globals[adjustedName] = { emittable, layout };

        return Value(emittable, layout);
    }

    detail::ValueTypeDescription LLVMContext::GetTypeImpl(Emittable emittable)
    {
        auto value = emittable.GetDataAs<LLVMValue>();
        auto type = value->getType();
        return LLVMTypeToValueType(type);
    }

    EmitterContext::DefinedFunction LLVMContext::CreateFunctionImpl(FunctionDeclaration decl, EmitterContext::DefinedFunction fn)
    {
        if (const auto& intrinsics = GetIntrinsics();
            std::find(intrinsics.begin(), intrinsics.end(), decl) != intrinsics.end())
        {
            throw InputException(InputExceptionErrors::invalidArgument, "Specified function is an intrinsic");
        }

        if (auto it = _definedFunctions.find(decl); it != _definedFunctions.end())
        {
            return it->second;
        }

        const auto& argValues = decl.GetParameterTypes();
        const auto& returnValue = decl.GetReturnType();

        std::vector<VariableType> variableArgTypes(argValues.size());
        std::transform(argValues.begin(), argValues.end(), variableArgTypes.begin(), [](Value value) {
            return ValueTypeToVariableType(value.GetBaseType());
        });

        const auto& fnName = decl.GetFunctionName();
        {
            ValueType returnValueType = returnValue ? returnValue->GetBaseType() : ValueType::Void;
            FunctionScope scope(*this, fnName, ValueTypeToVariableType(returnValueType), variableArgTypes);
            GetFunctionEmitter().SetAttributeForArguments(IRFunctionEmitter::Attributes::NoAlias);

            auto functionArgs = GetFunctionEmitter().Arguments();
            auto argValuesCopy = argValues;
            auto returnValueCopy = returnValue;

            for (std::pair idx{ 0u, functionArgs.begin() }; idx.first < argValuesCopy.size(); ++idx.first, ++idx.second)
            {
                idx.second->setName(std::string{ "arg" } + std::to_string(idx.first));
                argValuesCopy[idx.first].SetData(Emittable{ idx.second });
            }

            returnValueCopy = fn(argValuesCopy);
            if (returnValueCopy)
            {
                _emitter.EndFunction(EnsureEmittable(*returnValueCopy).Get<Emittable>().GetDataAs<LLVMValue>());
            }
            else
            {
                _emitter.EndFunction();
            }
        }

        DefinedFunction returnFn = [this, decl](std::vector<Value> args) -> std::optional<Value> {
            const auto& argValues = decl.GetParameterTypes();
            const auto& returnValue = decl.GetReturnType();
            const auto& fnName = decl.GetFunctionName();

            if (!std::equal(args.begin(),
                            args.end(),
                            argValues.begin(),
                            argValues.end(),
                            [](Value suppliedValue, Value fnValue) {
                                return suppliedValue.GetBaseType() == fnValue.GetBaseType();
                            }))
            {
                throw InputException(InputExceptionErrors::invalidArgument);
            }

            std::vector<LLVMValue> llvmArgs(args.size());
            std::transform(args.begin(), args.end(), llvmArgs.begin(), [this](Value& arg) {
                return EnsureEmittable(arg).Get<Emittable>().GetDataAs<LLVMValue>();
            });

            auto returnValueCopy = returnValue;
            LLVMValue fnReturnValue = _emitter.GetCurrentFunction().Call(fnName, llvmArgs);
            if (returnValueCopy)
            {
                returnValueCopy->SetData(Emittable{ fnReturnValue });
            }
            return returnValueCopy;
        };

        _definedFunctions[decl] = returnFn;

        return returnFn;
    }

    bool LLVMContext::IsFunctionDefinedImpl(FunctionDeclaration decl) const
    {
        if (const auto& intrinsics = GetIntrinsics();
            std::find(intrinsics.begin(), intrinsics.end(), decl) != intrinsics.end())
        {
            return true;
        }

        return _definedFunctions.find(decl) != _definedFunctions.end();
    }

    Value LLVMContext::StoreConstantDataImpl(ConstantData data) { return _computeContext.StoreConstantData(data); }

    void LLVMContext::ForImpl(MemoryLayout layout, std::function<void(std::vector<Scalar>)> fn)
    {
        std::vector<IRFunctionEmitter::ConstLoopRange> ranges(layout.NumDimensions());
        for (auto index = 0u; index < ranges.size(); ++index)
        {
            ranges[index].begin = 0;
            ranges[index].end = layout.GetActiveSize(index);
        }

        auto logicalOrder = layout.GetLogicalDimensionOrder();

        GetFunctionEmitter().For(
            ranges,
            [&fn, logicalOrder](emitters::IRFunctionEmitter&, std::vector<emitters::IRLocalScalar> indices) {
                std::vector<Scalar> logicalIndices(indices.size());
                for (int index = 0; index < static_cast<int>(indices.size()); ++index)
                {
                    logicalIndices[logicalOrder[index]] = Scalar(Value(indices[index].value, ScalarLayout));
                }
                fn(logicalIndices);
            });
    }

    void LLVMContext::ForImpl(Scalar start, Scalar stop, Scalar step, std::function<void(Scalar)> fn)
    {
        auto startValue = EnsureEmittable(start.GetValue());
        auto stopValue = EnsureEmittable(stop.GetValue());
        auto stepValue = EnsureEmittable(step.GetValue());

        auto& fnEmitter = GetFunctionEmitter();

        // IRFunctionEmitter::For returns a true scalar (not a pointer pointing to a single value)
        // It works better (especially with the rest of value library) if we pass in a pointer
        // instead. So, we allocate our own index and set it to the value of iterationVariable, below,
        // from IRFunctionEmitter and then use our own index when passing it into the functor provded
        Scalar index = value::Allocate<int>(ScalarLayout);

        fnEmitter.For(
            fnEmitter.Load(ToLLVMValue(startValue)),
            fnEmitter.Load(ToLLVMValue(stopValue)),
            fnEmitter.Load(ToLLVMValue(stepValue)),
            [&](IRFunctionEmitter&, IRLocalScalar iterationVariable) {
                index = Scalar(Value{ Emittable{ iterationVariable.value }, ScalarLayout });
                fn(index);
            });
    }

    void LLVMContext::MoveDataImpl(Value& source, Value& destination)
    {
        // we treat a move the same as a copy, except we clear out the source
        CopyDataImpl(source, destination);

        // data has been "moved", so clear the source
        source.Reset();
    }

    void LLVMContext::CopyDataImpl(const Value& source, Value& destination)
    {
        if (destination.IsConstant())
        {
            if (source.IsConstant())
            {
                return _computeContext.CopyData(source, destination);
            }
            else
            {
                auto emittableDestination = EnsureEmittable(destination);
                CopyDataImpl(source, emittableDestination);
                destination.SetData(emittableDestination);
            }
        }
        else
        {
            if (!TypeCompatible(destination, source) &&
                (destination.PointerLevel() == source.PointerLevel() ||
                 destination.PointerLevel() == (1 + source.PointerLevel())))
            {
                throw InputException(InputExceptionErrors::typeMismatch);
            }

            auto& irEmitter = _emitter.GetIREmitter();
            auto destValue = ToLLVMValue(destination);
            if (source.IsConstant())
            {
                // we're only copying active areas below. should we copy padded too?
                auto& layout = source.GetLayout();
                std::visit(
                    VariantVisitor{ [](Emittable) {},
                                    [destValue, &irEmitter, &layout](auto&& data) {
                                        ConstantForLoop(layout, [&](int offset) {
                                            auto srcAtOffset = irEmitter.Literal(*(data + offset));
                                            auto destOffset = irEmitter.Literal(offset);
                                            auto destAtOffset = irEmitter.PointerOffset(destValue, destOffset);
                                            (void)irEmitter.Store(destAtOffset, srcAtOffset);
                                        });
                                    } },
                    source.GetUnderlyingData());
            }
            else
            {
                auto srcValue = ToLLVMValue(source);
                if (srcValue == destValue)
                {
                    return;
                }
                if (auto& layout = source.GetLayout(); layout.IsContiguous())
                {
                    if (destination.PointerLevel() == source.PointerLevel())
                    {
                        auto llvmType = srcValue->getType()->getContainedType(0);
                        auto primSize = irEmitter.SizeOf(llvmType);
                        auto memorySize = irEmitter.Literal(static_cast<int64_t>(layout.GetMemorySize() * primSize));
                        irEmitter.MemoryCopy(srcValue,
                                             destValue,
                                             memorySize);
                    }
                    else
                    {
                        auto destAtOffset = irEmitter.PointerOffset(destValue, irEmitter.Zero(VariableType::Int32));
                        irEmitter.Store(destAtOffset, srcValue);
                    }
                }
                else
                {
                    ForImpl(layout, [&](std::vector<Scalar> index) {
                        auto offsetSource = source.Offset(detail::CalculateOffset(source.GetLayout(), index));
                        auto offsetDest = destination.Offset(detail::CalculateOffset(destination.GetLayout(), index));
                        (void)irEmitter.Store(ToLLVMValue(offsetDest), irEmitter.Load(ToLLVMValue(offsetSource)));
                    });
                }
            }
        }
    }

    Value LLVMContext::OffsetImpl(Value begin, Value index)
    {
        if (begin.IsConstant() && index.IsConstant())
        {
            return _computeContext.Offset(begin, index);
        }
        else
        {
            auto& fn = GetFunctionEmitter();
            Value emittableBegin = EnsureEmittable(begin);
            Value emittableIndex = EnsureEmittable(index);

            auto llvmBegin = std::get<Emittable>(emittableBegin.GetUnderlyingData()).GetDataAs<LLVMValue>();
            auto llvmIndex = std::get<Emittable>(emittableIndex.GetUnderlyingData()).GetDataAs<LLVMValue>();

            return Emittable{ fn.PointerOffset(llvmBegin, fn.ValueAt(llvmIndex, 0)) };
        }
    }

    Value LLVMContext::ReferenceImpl(Value sourceValue)
    {
        auto source = Realize(sourceValue);
        if (source.IsConstant())
        {
            return _computeContext.Reference(source);
        }

        auto llvmSourceValue = ToLLVMValue(source);

        auto& fn = GetFunctionEmitter();
        auto& irEmitter = fn.GetEmitter();

        auto llvmType = ValueTypeToLLVMType(irEmitter, { source.GetBaseType(), source.PointerLevel() });
        assert(llvmType->isPointerTy());
        auto allocatedVariable = fn.Variable(llvmType, 1);

        fn.SetValueAt(allocatedVariable, 0, llvmSourceValue);
        return { Emittable{ allocatedVariable }, source.GetLayout() };
    }

    Value LLVMContext::DereferenceImpl(Value sourceValue)
    {
        auto source = Realize(sourceValue);
        if (source.IsConstant())
        {
            return _computeContext.Dereference(source);
        }

        auto llvmSourceValue = ToLLVMValue(source);

        auto& fn = GetFunctionEmitter();
        auto& irEmitter = fn.GetEmitter();

        auto llvmLoadedValue = fn.ValueAt(llvmSourceValue);

        auto newPointerLevel = source.PointerLevel() - 1;
        auto llvmType = ValueTypeToLLVMType(irEmitter, { source.GetBaseType(), newPointerLevel });
        auto allocatedVariable = fn.Variable(llvmType, 1);

        fn.SetValueAt(allocatedVariable, 0, llvmLoadedValue);
        return { Emittable{ fn.ValueAt(allocatedVariable) }, source.GetLayout() };
    }

    Value LLVMContext::UnaryOperationImpl(ValueUnaryOperation op, Value destination)
    {
        throw LogicException(LogicExceptionErrors::notImplemented);
    }

    Value LLVMContext::BinaryOperationImpl(ValueBinaryOperation op, Value destination, Value source)
    {
        if (!source.IsDefined())
        {
            throw InputException(InputExceptionErrors::invalidArgument);
        }

        if (destination.IsDefined())
        {
            if (source.IsConstant() && destination.IsConstant())
            {
                return _computeContext.BinaryOperation(op, destination, source);
            }
        }
        else
        {
            destination = Allocate(source.GetBaseType(), source.GetLayout());
        }

        if (!TypeCompatible(destination, source) &&
            (destination.PointerLevel() == source.PointerLevel() ||
             destination.PointerLevel() == (1 + source.PointerLevel())))
        {
            throw InputException(InputExceptionErrors::typeMismatch);
        }

        if (destination.GetLayout() != source.GetLayout())
        {
            throw InputException(InputExceptionErrors::sizeMismatch);
        }

        auto& fn = GetFunctionEmitter();
        std::visit(
            [this, destination = EnsureEmittable(destination), &source, op, &fn](auto&& sourceData) {
                using SourceDataType = std::decay_t<decltype(sourceData)>;
                if constexpr (std::is_same_v<Boolean*, SourceDataType>)
                {
                    throw LogicException(LogicExceptionErrors::notImplemented);
                }
                else
                {
                    auto isFp = destination.IsFloatingPoint();
                    std::function<LLVMValue(LLVMValue, LLVMValue)> opFn;
                    switch (op)
                    {
                    case ValueBinaryOperation::add:
                        opFn = [&fn, isFp](auto dst, auto src) {
                            return fn.Operator(isFp ? TypedOperator::addFloat : TypedOperator::add, dst, src);
                        };
                        break;
                    case ValueBinaryOperation::subtract:
                        opFn = [&fn, isFp](auto dst, auto src) {
                            return fn.Operator(isFp ? TypedOperator::subtractFloat : TypedOperator::subtract, dst, src);
                        };
                        break;
                    case ValueBinaryOperation::multiply:
                        opFn = [&fn, isFp](auto dst, auto src) {
                            return fn.Operator(isFp ? TypedOperator::multiplyFloat : TypedOperator::multiply, dst, src);
                        };
                        break;
                    case ValueBinaryOperation::divide:
                        opFn = [&fn, isFp](auto dst, auto src) {
                            return fn.Operator(isFp ? TypedOperator::divideFloat : TypedOperator::divideSigned,
                                               dst,
                                               src);
                        };
                        break;
                    case ValueBinaryOperation::modulus:
                        if (isFp)
                        {
                            throw InputException(InputExceptionErrors::invalidArgument);
                        }
                        opFn = [&fn](auto dst, auto src) { return fn.Operator(TypedOperator::moduloSigned, dst, src); };
                        break;
                    default:
                        throw LogicException(LogicExceptionErrors::illegalState);
                    }

                    auto& layout = destination.GetLayout();
                    auto maxCoordinate = layout.GetActiveSize().ToVector();
                    decltype(maxCoordinate) coordinate(maxCoordinate.size());

                    if constexpr (std::is_same_v<Emittable, SourceDataType>)
                    {
                        // If the pointer levels don't match, it means the source is not a pointer (logically)
                        // and we just need to do an assignment of the value to the value pointed to by
                        // destintion
                        bool scalarLLVMSource = source.PointerLevel() != destination.PointerLevel();
                        ForImpl(layout, [&](std::vector<Scalar> index) {
                            LLVMValue srcValue = nullptr;
                            if (scalarLLVMSource)
                            {
                                srcValue = ToLLVMValue(source);
                            }
                            else
                            {
                                auto offsetSource = source.Offset(detail::CalculateOffset(source.GetLayout(), index));
                                srcValue = fn.Load(ToLLVMValue(offsetSource));
                            }
                            auto offsetDest = destination.Offset(detail::CalculateOffset(destination.GetLayout(), index));
                            auto destValue = ToLLVMValue(offsetDest);
                            fn.Store(destValue, opFn(fn.Load(destValue), srcValue));
                        });
                    }
                    else
                    {
                        auto destValue = ToLLVMValue(destination);
                        ConstantForLoop(layout, [&](int offset) {
                            auto offsetLiteral = fn.Literal(offset);
                            auto opResult = opFn(fn.ValueAt(destValue, offsetLiteral), fn.Literal(*(sourceData + offset)));
                            fn.SetValueAt(destValue, offsetLiteral, opResult);
                        });
                    }
                }
            },
            source.GetUnderlyingData());

        return destination;
    }

    Value LLVMContext::LogicalOperationImpl(ValueLogicalOperation op, Value source1, Value source2)
    {
        if (source1.GetLayout() != source2.GetLayout())
        {
            throw InputException(InputExceptionErrors::sizeMismatch);
        }

        if (source1.IsConstant() && source2.IsConstant())
        {
            return _computeContext.LogicalOperation(op, source1, source2);
        }

        auto comparisonOp = TypedComparison::none;
        bool isFp = source1.IsFloatingPoint();
        switch (op)
        {
        case ValueLogicalOperation::equality:
            comparisonOp = isFp ? TypedComparison::equalsFloat : TypedComparison::equals;
            break;
        case ValueLogicalOperation::inequality:
            comparisonOp = isFp ? TypedComparison::notEqualsFloat : TypedComparison::notEquals;
            break;
        case ValueLogicalOperation::greaterthan:
            comparisonOp = isFp ? TypedComparison::greaterThanFloat : TypedComparison::greaterThan;
            break;
        case ValueLogicalOperation::greaterthanorequal:
            comparisonOp = isFp ? TypedComparison::greaterThanOrEqualsFloat : TypedComparison::greaterThanOrEquals;
            break;
        case ValueLogicalOperation::lessthan:
            comparisonOp = isFp ? TypedComparison::lessThanFloat : TypedComparison::lessThan;
            break;
        case ValueLogicalOperation::lessthanorequal:
            comparisonOp = isFp ? TypedComparison::lessThanOrEqualsFloat : TypedComparison::lessThanOrEquals;
            break;
        }

        Value returnValue = std::visit(
            VariantVisitor{
                [this,
                 comparisonOp,
                 &source1,
                 &source2](Emittable source1Data) -> Value {
                    // source1 is an Emittable type, so source2 can be constant or Emittable
                    return std::visit(
                        VariantVisitor{
                            [&, this](Emittable source2Data) -> Value {
                                auto& fn = this->GetFunctionEmitter();
                                auto result = fn.TrueBit();

                                ForImpl(source1.GetLayout(), [&](std::vector<Scalar> index) {
                                    auto offsetSource1 = source1.Offset(detail::CalculateOffset(source1.GetLayout(), index));
                                    auto offsetSource2 = source2.Offset(detail::CalculateOffset(source2.GetLayout(), index));
                                    result = fn.LogicalAnd(
                                        result,
                                        fn.Comparison(
                                            comparisonOp,
                                            fn.Load(ToLLVMValue(offsetSource1)),
                                            fn.Load(ToLLVMValue(offsetSource2))));
                                });

                                return { Emittable{ result }, ScalarLayout };
                            },
                            [&, this](auto&& source2Data) -> Value {
                                using Type = std::remove_pointer_t<std::decay_t<decltype(source2Data)>>;
                                using CastType = std::conditional_t<std::is_same_v<Type, Boolean>, bool, Type>;
                                auto& fn = this->GetFunctionEmitter();

                                auto result = fn.TrueBit();
                                auto llvmOp1 = source1Data.GetDataAs<LLVMValue>();

                                ConstantForLoop(source1.GetLayout(), [&](const MemoryCoordinates& logicalCoordinates) {
                                    auto source1Offset = source1.GetLayout().GetLogicalEntryOffset(logicalCoordinates);
                                    auto source2Offset = source2.GetLayout().GetLogicalEntryOffset(logicalCoordinates);

                                    result = fn.LogicalAnd(
                                        result,
                                        fn.Comparison(
                                            comparisonOp,
                                            fn.ValueAt(llvmOp1, source1Offset),
                                            fn.Literal(static_cast<CastType>(source2Data[source2Offset]))));
                                });

                                return { Emittable{ result }, ScalarLayout };
                            } },
                        source2.GetUnderlyingData());
                },
                [this,
                 comparisonOp,
                 &source2Data = source2.GetUnderlyingData(),
                 &source1Layout = source1.GetLayout(),
                 &source2Layout = source2.GetLayout()](auto&& source1Data) -> Value {
                    // source1 is constant, so source2 has to be an Emittable type
                    using Type = std::remove_pointer_t<std::decay_t<decltype(source1Data)>>;
                    using CastType = std::conditional_t<std::is_same_v<Type, Boolean>, bool, Type>;

                    auto& fn = GetFunctionEmitter();
                    auto result = fn.TrueBit();
                    auto llvmOp2 = std::get<Emittable>(source2Data).GetDataAs<LLVMValue>();

                    ConstantForLoop(source1Layout, [&](const MemoryCoordinates& logicalCoordinates) {
                        auto source1Offset = source1Layout.GetLogicalEntryOffset(logicalCoordinates);
                        auto source2Offset = source2Layout.GetLogicalEntryOffset(logicalCoordinates);

                        result = fn.LogicalAnd(
                            result,
                            fn.Comparison(
                                comparisonOp,
                                fn.Literal(static_cast<CastType>(source1Data[source1Offset])),
                                fn.ValueAt(llvmOp2, source2Offset)));
                    });

                    return { Emittable{ result }, ScalarLayout };
                } },
            source1.GetUnderlyingData());

        return returnValue;
    }

    Value LLVMContext::CastImpl(Value value, ValueType type)
    {
        if (value.IsConstant())
        {
            return _computeContext.Cast(value, type);
        }

        auto data = ToLLVMValue(value);
        auto& fn = GetFunctionEmitter();

        auto castedData = Allocate(type, value.IsConstrained() ? value.GetLayout() : ScalarLayout);
        auto castedValue = ToLLVMValue(castedData);
        for (size_t index = 0u; index < castedData.GetLayout().GetMemorySize(); ++index)
        {
            fn.SetValueAt(
                castedValue,
                static_cast<int>(index),
                fn.CastValue(
                    fn.ValueAt(data, static_cast<int>(index)),
                    ValueTypeToLLVMType(fn.GetEmitter(), { type, 0 })));
        }

        return castedData;
    }

    class LLVMContext::IfContextImpl : public EmitterContext::IfContextImpl
    {
    public:
        IfContextImpl(IRIfEmitter ifEmitter, IRFunctionEmitter& fnEmitter) :
            _ifEmitter(std::move(ifEmitter)),
            _fnEmitter(fnEmitter)
        {}

        void ElseIf(Scalar test, std::function<void()> fn) override
        {
            LLVMValue testValue = nullptr;
            if (auto value = test.GetValue(); value.IsConstant())
            {
                testValue = _fnEmitter.Literal(static_cast<bool>(test.Get<Boolean>()));
            }
            else
            {
                testValue = ToLLVMValue(value);
            }

            _ifEmitter.ElseIf(testValue, [fn = std::move(fn)](auto&) { fn(); });
        }

        void Else(std::function<void()> fn) override
        {
            _ifEmitter.Else([fn = std::move(fn)](auto&) { fn(); });
        }

    private:
        IRIfEmitter _ifEmitter;
        IRFunctionEmitter& _fnEmitter;
    };

    EmitterContext::IfContext LLVMContext::IfImpl(Scalar test, std::function<void()> fn)
    {
        auto& fnEmitter = GetFunctionEmitter();
        LLVMValue testValue = nullptr;
        if (auto value = test.GetValue(); value.IsConstant())
        {
            testValue = fnEmitter.Literal(static_cast<bool>(test.Get<Boolean>()));
        }
        else
        {
            testValue = ToLLVMValue(value);
        }

        auto ifEmitter = fnEmitter.If(testValue, [fn = std::move(fn)](auto&) { fn(); });

        return { std::make_unique<LLVMContext::IfContextImpl>(std::move(ifEmitter), fnEmitter) };
    }

    std::optional<Value> LLVMContext::CallImpl(FunctionDeclaration func, std::vector<Value> args)
    {
        if (std::any_of(args.begin(), args.end(), [](const auto& value) { return value.IsEmpty(); }))
        {
            throw InputException(InputExceptionErrors::invalidArgument);
        }

        const auto& intrinsics = GetIntrinsics();
        if (std::find(intrinsics.begin(), intrinsics.end(), func) != intrinsics.end())
        {
            return IntrinsicCall(func, args);
        }

        if (auto it = _definedFunctions.find(func); it != _definedFunctions.end())
        {
            return it->second(args);
        }

        return EmitExternalCall(func, args);
    }
    void LLVMContext::PrefetchImpl(Value data, PrefetchType type, PrefetchLocality locality)
    {
        if (data.IsConstant())
        {
            // Maybe it's better to make the data emittable and prefetch it...
            return;
        }

        int localityNum = 0;
        switch (locality)
        {
        case PrefetchLocality::None:
            localityNum = 0;
            break;
        case PrefetchLocality::Low:
            localityNum = 1;
            break;
        case PrefetchLocality::Moderate:
            localityNum = 2;
            break;
        case PrefetchLocality::Extreme:
            localityNum = 3;
            break;
        default:
            throw LogicException(LogicExceptionErrors::illegalState);
        }

        int typeNum = type == PrefetchType::Read ? 0 : 1;

        auto& fnEmitter = GetFunctionEmitter();
        auto llvmData = fnEmitter.BitCast(ToLLVMValue(data), fnEmitter.GetEmitter().Type(VariableType::Char8Pointer));
        auto llvmType = fnEmitter.Literal(static_cast<int>(typeNum));
        auto llvmLocality = fnEmitter.Literal(static_cast<int>(localityNum));
        auto llvmCacheType = fnEmitter.Literal(1); // data cache prefetching

        auto prefetchFn = fnEmitter.GetModule().GetRuntime().GetPrefetchFunction();
        fnEmitter.Call(prefetchFn, { llvmData, llvmType, llvmLocality, llvmCacheType });
    }

    void LLVMContext::ParallelizeImpl(int numTasks, std::vector<Value> captured, std::function<void(Scalar, std::vector<Value>)> fn)
    {
        auto& fnEmitter = GetFunctionEmitter();

        std::vector<LLVMValue> capturedValues;
        capturedValues.reserve(captured.size());
        std::transform(
            captured.begin(),
            captured.end(),
            std::back_inserter(capturedValues),
            [this](Value& value) { return ToLLVMValue(EnsureEmittable(value)); });

        std::vector<Value> emptiedCaptures;
        emptiedCaptures.reserve(captured.size());
        std::transform(
            captured.begin(),
            captured.end(),
            std::back_inserter(emptiedCaptures),
            [](Value value) { value.ClearData(); return value; });

        fnEmitter.ParallelFor(
            numTasks,
            { numTasks },
            capturedValues,
            [this, fn = std::move(fn), emptied = std::move(emptiedCaptures)](IRFunctionEmitter& parFnEmitter, IRLocalScalar iteration, std::vector<LLVMValue> capturedValues) mutable {
                FunctionScope scope(*this, parFnEmitter);

                assert(emptied.size() == capturedValues.size());
                Value threadValue(Emittable{ iteration.value }, ScalarLayout);

                for (unsigned i = 0; i < emptied.size(); ++i)
                {
                    emptied[i].SetData(Emittable{ capturedValues[i] });
                }

                fn(threadValue, emptied);
            });
    }

    void LLVMContext::DebugDumpImpl(Value value, std::string tag, std::ostream& stream) const
    {
        auto realizedValue = Realize(value);
        if (realizedValue.IsConstant())
        {
            _computeContext.DebugDump(realizedValue, tag, &stream);
        }
        else
        {
            llvm::raw_os_ostream llvmStream(stream);
            auto llvmValue = std::get<Emittable>(value.GetUnderlyingData()).GetDataAs<LLVMValue>();
            emitters::DebugDump(llvmValue, tag, &llvmStream);
        }
    }

    void LLVMContext::DebugDumpImpl(FunctionDeclaration fn, std::string tag, std::ostream& stream) const
    {
        llvm::raw_os_ostream llvmStream(stream);

        auto& irEmitter = _emitter.GetIREmitter();

        const auto& fnName = fn.GetFunctionName();
        if (!_emitter.HasFunction(fnName))
        {
            const auto& argTypes = fn.GetParameterTypes();
            const auto& returnType = fn.GetReturnType();
            auto resultType = [&] {
                if (returnType)
                {
                    return ValueTypeToLLVMType(irEmitter, { returnType->GetBaseType(), returnType->PointerLevel() });
                }
                else
                {
                    return ValueTypeToLLVMType(irEmitter, { ValueType::Void, 0 });
                }
            }();

            std::vector<LLVMType> paramTypes(argTypes.size());
            std::transform(argTypes.begin(), argTypes.end(), paramTypes.begin(), [&](const auto& value) {
                return ValueTypeToLLVMType(irEmitter, { value.GetBaseType(), value.PointerLevel() });
            });

            auto fnType = llvm::FunctionType::get(resultType, paramTypes, false);
            _emitter.DeclareFunction(fnName, fnType);
        }
        auto llvmFn = _emitter.GetFunction(fnName);

        emitters::DebugDump(llvmFn, tag, &llvmStream);
    }

    void LLVMContext::DebugPrintImpl(std::string message)
    {
        _emitter.DebugPrint(message);
    }

    void LLVMContext::SetNameImpl(const Value& value, const std::string& name)
    {
        auto realized = Realize(value);
        if (realized.IsConstant())
        {
            return _computeContext.SetName(realized, name);
        }

        auto llvmValue = ToLLVMValue(realized);
        llvmValue->setName(name);
    }

    std::string LLVMContext::GetNameImpl(const Value& value) const
    {
        auto realized = Realize(value);
        if (realized.IsConstant())
        {
            return _computeContext.GetName(realized);
        }

        auto llvmValue = ToLLVMValue(realized);
        return llvmValue->getName();
    }

    Value LLVMContext::IntrinsicCall(FunctionDeclaration intrinsic, std::vector<Value> args)
    {
        static std::unordered_map<FunctionDeclaration, std::function<Value(IRFunctionEmitter&, std::vector<Value>)>> intrinsics = {
            { AbsFunctionDeclaration, SimpleNumericalFunctionIntrinsic(&IRRuntime::GetAbsFunction) },
            { CosFunctionDeclaration, SimpleNumericalFunctionIntrinsic(&IRRuntime::GetCosFunction) },
            { ExpFunctionDeclaration, SimpleNumericalFunctionIntrinsic(&IRRuntime::GetExpFunction) },
            { LogFunctionDeclaration, SimpleNumericalFunctionIntrinsic(&IRRuntime::GetLogFunction) },
            { Log10FunctionDeclaration, SimpleNumericalFunctionIntrinsic(&IRRuntime::GetLog10Function) },
            { Log2FunctionDeclaration, SimpleNumericalFunctionIntrinsic(&IRRuntime::GetLog2Function) },
            { MaxNumFunctionDeclaration, MaxMinIntrinsicFunction(MaxMinIntrinsic::Max) },
            { MinNumFunctionDeclaration, MaxMinIntrinsicFunction(MaxMinIntrinsic::Min) },
            { PowFunctionDeclaration, PowFunctionIntrinsic() },
            { SinFunctionDeclaration, SimpleNumericalFunctionIntrinsic(&IRRuntime::GetSinFunction) },
            { SqrtFunctionDeclaration, SimpleNumericalFunctionIntrinsic(&IRRuntime::GetSqrtFunction) },
            { TanhFunctionDeclaration, SimpleNumericalFunctionIntrinsic(&IRRuntime::GetTanhFunction) },
            { RoundFunctionDeclaration, SimpleNumericalFunctionIntrinsic(&IRRuntime::GetRoundFunction) },
            { FloorFunctionDeclaration, SimpleNumericalFunctionIntrinsic(&IRRuntime::GetFloorFunction) },
            { CeilFunctionDeclaration, SimpleNumericalFunctionIntrinsic(&IRRuntime::GetCeilFunction) },
            { CopySignFunctionDeclaration, CopySignFunctionIntrinsic() },
        };

        if (std::all_of(args.begin(), args.end(), [](const auto& value) { return value.IsConstant(); }))
        {
            // Compute context can handle intrinsic calls with constant data
            return *_computeContext.Call(intrinsic, args);
        }

        std::vector<Value> emittableArgs;
        emittableArgs.reserve(args.size());
        std::transform(args.begin(), args.end(), std::back_inserter(emittableArgs), [this](const auto& value) { return EnsureEmittable(value); });

        if (auto it = intrinsics.find(intrinsic); it != intrinsics.end())
        {
            return it->second(GetFunctionEmitter(), emittableArgs);
        }

        throw LogicException(LogicExceptionErrors::notImplemented);
    }

    std::optional<Value> LLVMContext::EmitExternalCall(FunctionDeclaration externalFunc, std::vector<Value> args)
    {
        const auto& argTypes = externalFunc.GetParameterTypes();
        if (args.size() != argTypes.size())
        {
            throw InputException(InputExceptionErrors::sizeMismatch);
        }

        auto& irEmitter = _emitter.GetIREmitter();
        auto& fnEmitter = GetFunctionEmitter();

        const auto& returnType = externalFunc.GetReturnType();

        // Create external function declaration
        const auto& fnName = externalFunc.GetFunctionName();
        if (!_emitter.HasFunction(fnName))
        {
            auto resultType = [&] {
                if (returnType)
                {
                    return ValueTypeToLLVMType(irEmitter, { returnType->GetBaseType(), returnType->PointerLevel() });
                }
                else
                {
                    return ValueTypeToLLVMType(irEmitter, { ValueType::Void, 0 });
                }
            }();

            std::vector<LLVMType> paramTypes(argTypes.size());
            std::transform(argTypes.begin(), argTypes.end(), paramTypes.begin(), [&](const auto& value) {
                return ValueTypeToLLVMType(irEmitter, { value.GetBaseType(), value.PointerLevel() });
            });

            auto fnType = llvm::FunctionType::get(resultType, paramTypes, false);
            _emitter.DeclareFunction(fnName, fnType);
        }
        auto fn = _emitter.GetFunction(fnName);

        // as a first approximation, if the corresponding arg type has a pointer level that's one less
        // than the passed in value, we dereference it. if it's the same, we pass it in as is. if it's anything else,
        // throw. this logic may not be sufficient for future use cases.
        std::vector<LLVMValue> argValues;
        argValues.reserve(args.size());
        for (auto idx = 0u; idx < args.size(); ++idx)
        {
            auto arg = EnsureEmittable(args[idx]);
            const auto& type = argTypes[idx];
            if (arg.GetBaseType() != type.GetBaseType())
            {
                throw InputException(InputExceptionErrors::typeMismatch);
            }

            if (arg.PointerLevel() == type.PointerLevel())
            {
                auto varg = ToLLVMValue(arg);
                argValues.push_back(varg);
            }
            else if (arg.PointerLevel() == (type.PointerLevel() + 1))
            {
                auto varg = ToLLVMValue(arg);
                argValues.push_back(fnEmitter.ValueAt(varg, 0));
            }
            else
            {
                throw InputException(InputExceptionErrors::typeMismatch);
            }
        }

        auto resultValue = fnEmitter.Call(fn, argValues);
        auto result = returnType;
        if (result)
        {
            if (result->PointerLevel() == 0)
            {
                result = value::Allocate(result->GetBaseType(), ScalarLayout);
                fnEmitter.SetValueAt(ToLLVMValue(*result), 0, resultValue);
            }
            else
            {
                result->SetData(Emittable{ resultValue });
            }
        }

        return result;
    }

    bool LLVMContext::TypeCompatible(Value value1, Value value2)
    {
        return value1.GetBaseType() == value2.GetBaseType();
    }

    std::string LLVMContext::GetScopeAdjustedName(GlobalAllocationScope scope, std::string name) const
    {
        switch (scope)
        {
        case GlobalAllocationScope::Global:
            return GetGlobalScopedName(name);
        case GlobalAllocationScope::Function:
            return GetCurrentFunctionScopedName(name);
        }

        throw LogicException(LogicExceptionErrors::illegalState);
    }

    std::string LLVMContext::GetGlobalScopedName(std::string name) const
    {
        return _emitter.GetModuleName() + "_" + name;
    }

    std::string LLVMContext::GetCurrentFunctionScopedName(std::string name) const
    {
        if (_functionStack.empty())
        {
            throw LogicException(LogicExceptionErrors::illegalState);
        }

        return GetGlobalScopedName(GetFunctionEmitter().GetFunctionName() + "_" + name);
    }

    IRFunctionEmitter& LLVMContext::GetFunctionEmitter() const
    {
        return _functionStack.top().get();
    }

    Value LLVMContext::PromoteConstantData(Value value)
    {
        assert(value.IsConstant() && value.IsDefined() && !value.IsEmpty());

        const auto& constantData = _computeContext.GetConstantData(value);

        ptrdiff_t offset = 0;
        LLVMValue llvmValue = std::visit(
            [this, &value, &offset](auto&& data) -> LLVMValue {
                using Type = std::decay_t<decltype(data)>;
                using DataType = typename Type::value_type;

                auto ptrData = std::get<DataType*>(value.GetUnderlyingData());
                offset = ptrData - data.data();

                if (_functionStack.empty())
                {
                    std::string globalName =
                        GetGlobalScopedName("_"s + std::to_string(_promotedConstantStack.top().size()));

                    llvm::GlobalVariable* globalVariable = nullptr;
                    if constexpr (std::is_same_v<DataType, Boolean>)
                    {
                        globalVariable =
                            _emitter.GlobalArray(globalName, std::vector<uint8_t>(data.begin(), data.end()));
                    }
                    else
                    {
                        globalVariable = _emitter.GlobalArray(globalName, data);
                    }

                    return globalVariable;
                }
                else
                {
                    auto& fn = GetFunctionEmitter();

                    std::string globalName = GetCurrentFunctionScopedName("_"s + std::to_string(_promotedConstantStack.top().size()));

                    llvm::GlobalVariable* globalVariable = nullptr;
                    if constexpr (std::is_same_v<DataType, Boolean>)
                    {
                        globalVariable =
                            _emitter.GlobalArray(globalName, std::vector<uint8_t>(data.begin(), data.end()));
                    }
                    else
                    {
                        globalVariable = _emitter.GlobalArray(globalName, data);
                    }

                    auto varType =
                        GetVariableType<std::conditional_t<std::is_same_v<DataType, Boolean>, bool, DataType>>();

                    LLVMValue newValue = fn.Variable(varType, data.size());
                    fn.MemoryCopy<DataType>(globalVariable, newValue, static_cast<int>(data.size()));

                    return newValue;
                }
            },
            constantData);

        _promotedConstantStack.top().push_back({ &constantData, llvmValue });

        auto& irEmitter = _emitter.GetIREmitter();
        auto llvmOffset = irEmitter.Literal(static_cast<int>(offset));
        if (auto globalVariable = llvm::dyn_cast<llvm::GlobalVariable>(llvmValue))
        {
            llvmValue = irEmitter.PointerOffset(globalVariable, llvmOffset);
        }
        else
        {
            llvmValue = irEmitter.PointerOffset(llvmValue, llvmOffset);
        }

        Value newValue = value;
        newValue.SetData(Emittable{ llvmValue });

        return newValue;
    }

    std::optional<LLVMContext::PromotedConstantDataDescription> LLVMContext::HasBeenPromoted(Value value) const
    {
        if (!value.IsDefined() || value.IsEmpty() || !value.IsConstant())
        {
            return std::nullopt;
        }

        const auto& constantData = _computeContext.GetConstantData(value);
        const auto& promotedStack = _promotedConstantStack.top();

        if (auto it = std::find_if(promotedStack.begin(),
                                   promotedStack.end(),
                                   [&constantData](const auto& desc) { return desc.data == &constantData; });
            it != promotedStack.end())
        {
            return *it;
        }
        else
        {
            return std::nullopt;
        }
    }

    Value LLVMContext::Realize(Value value) const
    {
        if (auto desc = HasBeenPromoted(value); !desc)
        {
            return value;
        }
        else
        {
            const auto& promotionalDesc = *desc;
            auto offset = std::visit(
                [&value](auto&& data) -> ptrdiff_t {
                    using Type = std::decay_t<decltype(data)>;
                    using DataType = typename Type::value_type;

                    auto ptrData = std::get<DataType*>(value.GetUnderlyingData());

                    return ptrData - data.data();
                },
                *promotionalDesc.data);

            auto& fn = GetFunctionEmitter();
            auto emittable = promotionalDesc.realValue;
            auto name = _computeContext.GetName(value);

            Value newValue = value;
            newValue.SetData(Emittable{ fn.PointerOffset(emittable.GetDataAs<LLVMValue>(), static_cast<int>(offset)) });
            if (!name.empty())
            {
                ToLLVMValue(newValue)->setName(name);
            }

            return newValue;
        }
    }

    Value LLVMContext::EnsureEmittable(Value value)
    {
        if (!value.IsConstant())
        {
            return value;
        }
        else if (Value newValue = Realize(value); !newValue.IsConstant())
        {
            return newValue;
        }
        else
        {
            return PromoteConstantData(newValue);
        }
    }

} // namespace value
} // namespace ell
