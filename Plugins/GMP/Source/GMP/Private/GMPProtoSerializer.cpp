//  Copyright GenericMessagePlugin, Inc. All Rights Reserved.

#include "GMPProtoSerializer.h"

#include "GMPProtoUtils.h"
#include "HAL/PlatformFile.h"
#include "Serialization/MemoryReader.h"
#include "Serialization/MemoryWriter.h"
#include "UnrealCompatibility.h"
#include "upb/libupb.h"

#include <variant>

#if WITH_EDITOR
namespace upb
{
namespace generator
{
	struct FPreGenerator
	{
		FDynamicArena Arena;
		TArray<upb_StringView> Descriptors;

		using FNameType = FString;

		TMap<const FDefPool::FProtoDescType*, FNameType> ProtoNames;
		TMap<FNameType, const FDefPool::FProtoDescType*> ProtoMap;
		TMap<const FDefPool::FProtoDescType*, upb_StringView> ProtoDescs;

		TMap<FNameType, TArray<FNameType>> ProtoDeps;

		void PreAddProtoDesc(upb_StringView Buf) { Descriptors.Add(Buf); }
		void PreAddProtoDesc(TArrayView<uint8> Buf) { Descriptors.Add(Arena.AllocString(Buf)); }

		bool PreAddProto(upb_StringView Buf)
		{
			auto Proto = FDefPool::ParseProto(Buf, Arena);
			if (Proto && !ProtoNames.Contains(Proto))
			{
				ProtoDescs.Add(Proto, Buf);
				FNameType Name = StringView(FDefPool::GetProtoName(Proto));
				ProtoNames.Add(Proto, Name);
				ProtoMap.Add(Name, Proto);

				auto& Arr = ProtoDeps.FindOrAdd(Name);
				for (auto DepName : FDefPool::GetProtoDepencies(Proto))
					Arr.Add(StringView(DepName));
				return true;
			}

			return false;
		}

		void Reset()
		{
			Descriptors.Empty();
			ProtoNames.Empty();
			ProtoMap.Empty();
			ProtoDescs.Empty();
			ProtoDeps.Empty();
			Arena = FArena();
		}

		TArray<const FDefPool::FProtoDescType*> GenerateProtoList() const
		{
			for (auto& Elm : Descriptors)
			{
				const_cast<FPreGenerator*>(this)->PreAddProto(Elm);
			}

			TArray<FNameType> ResultNames;
			for (auto& Pair : ProtoDeps)
			{
				AddProtoImpl(ResultNames, Pair.Key);
			}
			TArray<const FDefPool::FProtoDescType*> Ret;
			for (auto& Name : ResultNames)
			{
				Ret.Add(ProtoMap.FindChecked(Name));
			}
			return Ret;
		}

		void AddProtoImpl(TArray<FNameType>& Results, const FNameType& ProtoName) const
		{
			if (Results.Contains(ProtoName))
			{
				return;
			}
			auto Names = ProtoDeps.FindChecked(ProtoName);
			for (auto i = 0; i < Names.Num(); ++i)
			{
				AddProtoImpl(Results, Names[i]);
			}
			Results.Add(ProtoName);
		}

		TArray<FFileDefPtr> FillDefPool(FDefPool& Pool, TMap<const upb_FileDef*, upb_StringView>& OutMap)
		{
			auto ProtoList = GenerateProtoList();
			TArray<FFileDefPtr> FileDefs;
			for (auto Proto : ProtoList)
			{
				if (auto FileDef = Pool.AddProto(Proto))
				{
					FileDefs.Add(FileDef);
					OutMap.Emplace(*FileDef, ProtoDescs.FindChecked(Proto));
				}
			}
			return FileDefs;
		}
	};
	static FPreGenerator& GetPreGenerator()
	{
		static FPreGenerator PreGenerator;
		return PreGenerator;
	}
	static TArray<FFileDefPtr> FillDefPool(FDefPool& Pool, TMap<const upb_FileDef*, upb_StringView>& OutMap)
	{
		return GetPreGenerator().FillDefPool(Pool, OutMap);
	}
	bool upbRegFileDescProtoImpl(const _upb_DefPool_Init* DefInit)
	{
		return GetPreGenerator().PreAddProto(DefInit->descriptor);
	}
}  // namespace generator
}  // namespace upb
#endif  // WITH_EDITOR

namespace GMP
{
namespace PB
{
	using namespace upb;
	struct FGMPDefPool
	{
		FDefPool DefPool;
		TMap<FName, FMessageDefPtr> MsgDefs_;
		bool AddProto(const FDefPool::FProtoDescType* FileProto)
		{
			FStatus Status;
			auto FileDef = DefPool.AddProto(FileProto, Status);
			MapProtoName(FileDef);
			return Status.IsOk();
		}
		FMessageDefPtr FindMessageByStruct(const UScriptStruct* Struct)
		{
			if (auto ProtoStruct = Cast<UProtoDefinedStruct>(Struct))
			{
				return DefPool.FindMessageByName(StringView::Ref(ProtoStruct->FullName));
			}

			ensure(Struct->IsNative());
			if (auto Find = MsgDefs_.Find(Struct->GetFName()))
			{
				return *Find;
			}

			return DefPool.FindMessageByName(StringView::Ref(Struct->GetName()));
		}

		void MapProtoName(FFileDefPtr FileDef)
		{
			if (!FileDef || FileDef.ToplevelMessageCount() == 0)
				return;

			FArena Arena;
			for (auto i = 0; i < FileDef.ToplevelMessageCount(); ++i)
			{
				auto Msg = FileDef.ToplevelMessage(i);
				if (ensure(Msg))
				{
#if 0
					FString FullName = Msg.FullName().ToFString();
#if 1
					FullName.ReplaceCharInline(TEXT('.'), TEXT('_'), ESearchCase::CaseSensitive);
#else
					FullName = FullName.Replace(TEXT("."), TEXT("_"), ESearchCase::CaseSensitive);
#endif
					MsgDefs_.Add(*FullName, Msg);
#endif
					MsgDefs_.Add(Msg.Name(), Msg);
				}
			}
		}
	};

	static auto& GetDefPoolMap()
	{
		static TMap<uint8, TUniquePtr<FGMPDefPool>> PoolMap;
		return PoolMap;
	}

	static TUniquePtr<FGMPDefPool>& ResetDefPool(uint8 Idx = 0)
	{
		auto& Ref = GetDefPoolMap().FindOrAdd(Idx);
		Ref = MakeUnique<FGMPDefPool>();
		return Ref;
	}
	static TUniquePtr<FGMPDefPool>& GetDefPool(uint8 Idx = 0)
	{
		auto Find = GetDefPoolMap().Find(Idx);
		if (!Find)
		{
			Find = &ResetDefPool(Idx);
#if WITH_EDITOR
			auto& PreGenerator = upb::generator::GetPreGenerator();
			auto ProtoList = PreGenerator.GenerateProtoList();
			for (auto Proto : ProtoList)
			{
				(*Find)->AddProto(Proto);
			}
#endif  // WITH_EDITOR
		}
		return *Find;
	}

	FMessageDefPtr FindMessageByStruct(const UScriptStruct* Struct)
	{
		return GetDefPool()->FindMessageByStruct(Struct);
	}

	bool AddProto(const char* InBuf, uint32 InSize)
	{
		FArena Arena;
		auto FileProto = FDefPool::ParseProto(StringView(InBuf, InSize), *Arena);
		return GetDefPool()->AddProto(FileProto);
	}

	bool AddProtos(const char* InBuf, uint32 InSize)
	{
		size_t DefCnt = 0;
		auto Arena = FArena();
		auto& Pair = *GetDefPool();
		FDefPool::IteratorProtoSet(FDefPool::ParseProtoSet(upb_StringView_FromDataAndSize(InBuf, InSize), Arena), [&](auto* FileProto) { DefCnt += Pair.AddProto(FileProto) ? 1 : 0; });
		return DefCnt > 0;
	}
	void ClearProtos()
	{
		GetDefPoolMap().Empty();
	}

	//////////////////////////////////////////////////////////////////////////
	struct PBEnum
	{
		int32_t EnumValue;
	};
	template<typename... Ts>
	struct Overload : Ts...
	{
		using Ts::operator()...;
	};
	template<class... Ts>
	Overload(Ts...) -> Overload<Ts...>;

	template<typename... TArgs>
	using TValueType = std::variant<std::monostate, bool, int32, uint32, int64, uint64, float, double, TArgs...>;
	template<typename T>
	struct TBaseFieldInfo
	{
		// static FFieldDefPtr GetFieldDef() const { return FieldDef; }
		// static bool EqualType(upb_CType InType) { return false; }
	};
	template<>
	struct TBaseFieldInfo<bool>
	{
		static constexpr upb_CType CType = upb_CType::kUpb_CType_Bool;
		static constexpr upb_CType CompactType = upb_CType::kUpb_CType_Bool;
		static bool EqualType(upb_CType InType) { return InType == CType || InType == CompactType; }
		static bool EqualField(FFieldDefPtr FieldDef) { return FieldDef.IsPrimitive() && EqualType(FieldDef.GetCType()); }
	};
	template<>
	struct TBaseFieldInfo<float>
	{
		static constexpr upb_CType CType = upb_CType::kUpb_CType_Float;
		static constexpr upb_CType CompactType = upb_CType::kUpb_CType_Float;
		static bool EqualType(upb_CType InType) { return InType == CType || InType == CompactType; }
		static bool EqualField(FFieldDefPtr FieldDef) { return FieldDef.IsPrimitive() && EqualType(FieldDef.GetCType()); }
	};
	template<>
	struct TBaseFieldInfo<double>
	{
		static constexpr upb_CType CType = upb_CType::kUpb_CType_Double;
		static constexpr upb_CType CompactType = upb_CType::kUpb_CType_Double;
		static bool EqualType(upb_CType InType) { return InType == CType || InType == CompactType; }
		static bool EqualField(FFieldDefPtr FieldDef) { return FieldDef.IsPrimitive() && EqualType(FieldDef.GetCType()); }
	};
	template<>
	struct TBaseFieldInfo<int32>
	{
		static constexpr upb_CType CType = upb_CType::kUpb_CType_Int32;
		static constexpr upb_CType CompactType = upb_CType::kUpb_CType_UInt32;
		static bool EqualType(upb_CType InType) { return InType == CType || InType == CompactType; }
		static bool EqualField(FFieldDefPtr FieldDef) { return FieldDef.IsPrimitive() && EqualType(FieldDef.GetCType()); }
	};
	template<>
	struct TBaseFieldInfo<uint32>
	{
		static constexpr upb_CType CType = upb_CType::kUpb_CType_UInt32;
		static constexpr upb_CType CompactType = upb_CType::kUpb_CType_Int32;
		static bool EqualType(upb_CType InType) { return InType == CType || InType == CompactType; }
		static bool EqualField(FFieldDefPtr FieldDef) { return FieldDef.IsPrimitive() && EqualType(FieldDef.GetCType()); }
	};
	template<>
	struct TBaseFieldInfo<int64>
	{
		static constexpr upb_CType CType = upb_CType::kUpb_CType_Int64;
		static constexpr upb_CType CompactType = upb_CType::kUpb_CType_UInt64;
		static bool EqualType(upb_CType InType) { return InType == CType || InType == CompactType; }
		static bool EqualField(FFieldDefPtr FieldDef) { return FieldDef.IsPrimitive() && EqualType(FieldDef.GetCType()); }
	};
	template<>
	struct TBaseFieldInfo<uint64>
	{
		static constexpr upb_CType CType = upb_CType::kUpb_CType_UInt64;
		static constexpr upb_CType CompactType = upb_CType::kUpb_CType_Int64;
		static bool EqualType(upb_CType InType) { return InType == CType || InType == CompactType; }
		static bool EqualField(FFieldDefPtr FieldDef) { return FieldDef.IsPrimitive() && EqualType(FieldDef.GetCType()); }
	};
	template<>
	struct TBaseFieldInfo<PBEnum>
	{
		static constexpr upb_CType CType = upb_CType::kUpb_CType_Enum;
		static constexpr upb_CType CompactType = upb_CType::kUpb_CType_Int32;
		static bool EqualType(upb_CType InType) { return InType == CType || InType == CompactType; }
		static bool EqualField(FFieldDefPtr FieldDef) { return FieldDef.IsPrimitive() && EqualType(FieldDef.GetCType()); }
	};
	template<>
	struct TBaseFieldInfo<upb_StringView>
	{
		static constexpr upb_CType CType = upb_CType::kUpb_CType_String;
		static constexpr upb_CType CompactType = upb_CType::kUpb_CType_Bytes;
		static bool EqualType(upb_CType InType) { return InType == CType || InType == CompactType; }
		static bool EqualField(FFieldDefPtr FieldDef) { return FieldDef.IsPrimitive() && EqualType(FieldDef.GetCType()); }
	};
	template<>
	struct TBaseFieldInfo<upb_Message*>
	{
		static bool EqualField(FFieldDefPtr FieldDef) { return FieldDef.IsSubMessage(); }
	};
	template<>
	struct TBaseFieldInfo<upb_Map*>
	{
		static bool EqualField(FFieldDefPtr FieldDef) { return FieldDef.IsMap(); }
	};
	template<>
	struct TBaseFieldInfo<upb_Array*>
	{
		static bool EqualField(FFieldDefPtr FieldDef) { return FieldDef.IsRepeated(); }
	};

	using FMessageVariant = TValueType<upb_StringView, upb_Message*, upb_Array*, upb_Map*>;
	auto AsVariant(const upb_MessageValue& Val, FFieldDefPtr FieldDef) -> FMessageVariant
	{
		if (FieldDef.IsMap())
		{
			return const_cast<upb_Map*>(Val.map_val);
		}
		else if (FieldDef.IsRepeated())
		{
			return const_cast<upb_Array*>(Val.array_val);
		}

		switch (FieldDef.GetCType())
		{
			// clang-format off
			case kUpb_CType_Bool: return Val.bool_val;
			case kUpb_CType_Float: return Val.float_val;
			case kUpb_CType_Enum: case kUpb_CType_Int32: return Val.int32_val;
			case kUpb_CType_UInt32: return Val.uint32_val;
			case kUpb_CType_Double: return Val.double_val;
			case kUpb_CType_Int64: return Val.int64_val;
			case kUpb_CType_UInt64: return Val.uint64_val;
			case kUpb_CType_String: case kUpb_CType_Bytes:return Val.str_val; 
			case kUpb_CType_Message: default: return const_cast<upb_Message*>(Val.msg_val);
				// clang-format on
		}
	}

	bool FromVariant(upb_MessageValue& OutVal, FFieldDefPtr FieldDef, const FMessageVariant& Var)
	{
		bool bRet = false;
		// clang-format off
		std::visit(Overload{
			[&](bool val) { OutVal.bool_val = val; bRet = TBaseFieldInfo<decltype(val)>::EqualField(FieldDef); },
			[&](float val) { OutVal.float_val = val; bRet = TBaseFieldInfo<decltype(val)>::EqualField(FieldDef); },
			[&](double val) { OutVal.double_val = val; bRet = TBaseFieldInfo<decltype(val)>::EqualField(FieldDef); },
			[&](int32 val) { OutVal.int32_val = val; bRet = TBaseFieldInfo<decltype(val)>::EqualField(FieldDef); },
			[&](uint32 val) { OutVal.uint32_val = val; bRet = TBaseFieldInfo<decltype(val)>::EqualField(FieldDef); },
			[&](int64 val) { OutVal.int64_val = val; bRet = TBaseFieldInfo<decltype(val)>::EqualField(FieldDef); },
			[&](uint64 val) { OutVal.uint64_val = val; bRet = TBaseFieldInfo<decltype(val)>::EqualField(FieldDef); },
			[&](upb_StringView val) { OutVal.str_val = val; bRet = TBaseFieldInfo<decltype(val)>::EqualField(FieldDef); },
			[&](upb_Array* val) { OutVal.array_val = val; bRet = TBaseFieldInfo<decltype(val)>::EqualField(FieldDef); },
			[&](upb_Map* val) { OutVal.map_val = val; bRet = TBaseFieldInfo<decltype(val)>::EqualField(FieldDef); },
			[&](auto) { }
			}, Var);
		// clang-format on
		return ensure(bRet);
	}

	struct FProtoReader
	{
		FFieldDefPtr FieldDef;
		FMessageVariant Var;
		FProtoReader(const FMessageVariant& InVar, FFieldDefPtr InField)
			: FieldDef(InField)
			, Var(InVar)
		{
		}
		FProtoReader(const upb_Message* InMsg, FFieldDefPtr InField)
			: FProtoReader(FMessageVariant(const_cast<upb_Message*>(InMsg)), InField)
		{
		}

		FProtoReader(upb_MessageValue InVal, FFieldDefPtr InField)
			: FieldDef(InField)
			, Var(AsVariant(InVal, InField))
		{
		}

		FProtoReader(FFieldDefPtr InField)
			: FieldDef(InField)
		{
		}

		template<typename T>
		bool GetValue(T& OutVar) const
		{
			if (std::holds_alternative<T>(Var))
			{
				OutVar = std::get<T>(Var);
				return true;
			}
			return false;
		}
		bool IsContainer() const { return std::holds_alternative<upb_Message*>(Var) || std::holds_alternative<upb_Map*>(Var) || std::holds_alternative<upb_Array*>(Var); }

		const upb_Message* GetMsg() const { return std::get<upb_Message*>(Var); }
		const upb_Array* GetArr() const { return std::get<upb_Array*>(Var); }
		const upb_Map* GetMap() const { return std::get<upb_Map*>(Var); }

		const upb_MiniTable* MiniTable() const { return FieldDef.ContainingType().MiniTable(); }

		//////////////////////////////////////////////////////////////////////////
		bool IsEnum() const { return FieldDef.GetCType() == upb_CType::kUpb_CType_Enum; }

		//////////////////////////////////////////////////////////////////////////
		bool IsBool() const { return FieldDef.GetCType() == upb_CType::kUpb_CType_Bool; }
		bool IsFloat() const { return FieldDef.GetCType() == upb_CType::kUpb_CType_Float; }
		bool IsDouble() const { return FieldDef.GetCType() == upb_CType::kUpb_CType_Double; }
		bool IsInt() const { return FieldDef.GetCType() == upb_CType::kUpb_CType_Int32; }
		bool IsUint() const { return FieldDef.GetCType() == upb_CType::kUpb_CType_UInt32; }
		bool IsInt64() const { return FieldDef.GetCType() == upb_CType::kUpb_CType_Int64; }
		bool IsUint64() const { return FieldDef.GetCType() == upb_CType::kUpb_CType_UInt64; }
		bool IsNumber() const { return IsBool() || IsFloat() || IsDouble() || IsInt() || IsUint() || IsInt64() || IsUint64() || IsEnum(); }

		template<typename T>
		void GetFieldNum(T& Out) const
		{
			if (ensureAlways(TBaseFieldInfo<T>::EqualType(FieldDef.GetCType())))
			{
				if (FieldDef.GetArrayIdx() < 0)
				{
					auto DefaultVal = FieldDef.DefaultValue();
					_upb_Message_GetNonExtensionField(GetMsg(), FieldDef.MiniTable(), &DefaultVal, &Out);
				}
				else if (ensureAlways(FieldDef.GetArrayIdx() < ArraySize()))
				{
					Out = *(const T*)ArrayElmData();
				}
			}
		}

		template<typename T>
		T GetFieldNum() const
		{
			T Ret;
			GetFieldNum(Ret);
			return Ret;
		}
		TValueType<> AsNumber() const
		{
			if (IsBool())
			{
				return GetFieldNum<bool>();
			}
			else if (IsFloat())
			{
				return GetFieldNum<float>();
			}
			else if (IsDouble())
			{
				return GetFieldNum<double>();
			}
			else if (IsInt())
			{
				return GetFieldNum<int32>();
			}
			else if (IsUint())
			{
				return GetFieldNum<uint32>();
			}
			else if (IsInt64())
			{
				return GetFieldNum<int64>();
			}
			else if (IsUint64())
			{
				return GetFieldNum<uint64>();
			}
			else if (IsEnum())
			{
				return GetFieldNum<int32>();
			}
			return std::monostate{};
		}
		template<typename T, typename F>
		T ToNumber(const F& Func) const
		{
			ensure(IsNumber());
			T Ret{};
			std::visit([&](const auto& Item) { Ret = Func(Item); }, AsNumber());
			return Ret;
		}

		template<typename T>
		T ToNumber() const
		{
			ensure(IsNumber());
			T Ret{};
			std::visit([&](const auto& Item) { Ret = VisitVal<T>(Item); }, AsNumber());
			return Ret;
		}
		//////////////////////////////////////////////////////////////////////////
		bool IsBytes() const { return FieldDef.GetCType() == upb_CType::kUpb_CType_Bytes; }

		//////////////////////////////////////////////////////////////////////////
		bool IsString() const { return FieldDef.GetCType() == upb_CType::kUpb_CType_String; }
		template<typename T>
		void GetFieldStr(T& Out) const
		{
			if (ensureAlways(IsString()))
			{
				if (FieldDef.GetArrayIdx() < 0)
				{
					auto val = upb_Message_GetString(GetMsg(), FieldDef.MiniTable(), FieldDef.DefaultValue().str_val);
					Out = StringView(val);
				}
				else if (ensureAlways(FieldDef.GetArrayIdx() < ArraySize()))
				{
					const upb_Array* arr = upb_Message_GetArray(GetMsg(), FieldDef.MiniTable());
					if (arr && ensure(FieldDef.GetArrayIdx() < arr->size))
					{
						Out = StringView(*(const upb_StringView*)ArrayElmData());
					}
					else
					{
						Out = {};
					}
				}
			}
		}

		upb_StringView GetFieldBytes() const
		{
			if (ensureAlways(IsString()))
			{
				if (FieldDef.GetArrayIdx() < 0)
				{
					return upb_Message_GetString(GetMsg(), FieldDef.MiniTable(), FieldDef.DefaultValue().str_val);
				}
				else if (ensureAlways(FieldDef.GetArrayIdx() < ArraySize()))
				{
					const upb_Array* arr = upb_Message_GetArray(GetMsg(), FieldDef.MiniTable());
					if (arr && ensure(FieldDef.GetArrayIdx() < arr->size))
					{
						return (*(const upb_StringView*)ArrayElmData());
					}
				}
			}
			return {};
		}

		template<typename T = StringView>
		T GetFieldStr() const
		{
			T Ret{};
			GetFieldStr(Ret);
			return Ret;
		}
		//////////////////////////////////////////////////////////////////////////
		bool IsArray() const { return FieldDef.IsRepeated(); }
		size_t ArraySize() const
		{
			const upb_Array* arr = IsArray() ? upb_Message_GetArray(GetMsg(), FieldDef.MiniTable()) : nullptr;
			return arr ? arr->size : 0;
		}

		const FProtoReader ArrayElm(size_t Idx) const
		{
			GMP_CHECK(IsArray());
			const upb_Array* arr = upb_Message_GetArray(GetMsg(), FieldDef.MiniTable());
			ensureAlways(arr && Idx < arr->size);
			return FProtoReader(GetMsg(), FieldDef.GetElementDef(Idx));
		}

		//////////////////////////////////////////////////////////////////////////
		bool IsMessage() const { return FieldDef.IsSubMessage(); }

		//////////////////////////////////////////////////////////////////////////
		bool IsMap() const { return FieldDef.IsMap(); }
		FMessageDefPtr MapEntryDef() const
		{
			GMP_CHECK(IsMap());
			return FieldDef.MapEntrySubdef();
		}
		const upb_Map* GetSubMap() const
		{
			GMP_CHECK(IsMap());
			return upb_Message_GetMap(GetMsg(), FieldDef.MiniTable());
		}
		//////////////////////////////////////////////////////////////////////////
		TValueType<StringView, const FProtoReader*> DispatchFieldValue() const
		{
			if (IsContainer())
			{
				if (FieldDef.GetArrayIdx() < 0)
				{
					GMP_CHECK(IsMessage() || IsArray() || IsMap());
					return this;
				}
				else if (IsString())
				{
					return GetFieldStr<StringView>();
				}
				else if (IsBool())
				{
					return GetFieldNum<bool>();
				}
				else if (IsFloat())
				{
					return GetFieldNum<float>();
				}
				else if (IsDouble())
				{
					return GetFieldNum<double>();
				}
				else if (IsInt())
				{
					return GetFieldNum<int32>();
				}
				else if (IsUint())
				{
					return GetFieldNum<uint32>();
				}
				else if (IsInt64())
				{
					return GetFieldNum<int64>();
				}
				else if (IsUint64())
				{
					return GetFieldNum<uint64>();
				}
				else if (IsEnum())
				{
					return GetFieldNum<int32>();
				}
			}
			else
			{
				TValueType<StringView, const FProtoReader*> Ret = std::monostate{};
				std::visit(Overload{[&](bool val) { Ret = val; },
									[&](float val) { Ret = val; },
									[&](double val) { Ret = val; },
									[&](int32 val) { Ret = val; },
									[&](uint32 val) { Ret = val; },
									[&](int64 val) { Ret = val; },
									[&](uint64 val) { Ret = val; },
									[&](upb_StringView val) { Ret = StringView(val); },
									[&](auto) { ensure(false); }},
						   Var);
				return Ret;
			}
			return std::monostate{};
		}

		const upb_Message* GetSubMessage() const
		{
			if (ensureAlways(IsMessage()))
			{
				auto DefautVal = FieldDef.DefaultValue();
				if (FieldDef.GetArrayIdx() < 0)
				{
					return upb_Message_GetMessage(GetMsg(), FieldDef.MiniTable(), &DefautVal);
				}
				else if (ensureAlways(FieldDef.GetArrayIdx() < ArraySize()))
				{
					return (const upb_Message* const*)ArrayElmData();
				}
			}
			return nullptr;
		}

	protected:
		const void* ArrayElmData(size_t Idx) const
		{
			GMP_CHECK(IsArray());
			const upb_Array* arr = upb_Message_GetArray(GetMsg(), FieldDef.MiniTable());
			ensureAlways(arr && Idx < arr->size);
			return (const char*)upb_Array_DataPtr(arr) + _upb_Array_ElementSizeLg2(arr) * Idx;
		}
		const void* ArrayElmData() const
		{
			GMP_CHECK(FieldDef.GetArrayIdx() >= 0);
			return ArrayElmData(FieldDef.GetArrayIdx());
		}
		template<typename V>
		static V VisitVal(const std::monostate& Val)
		{
			return {};
		}
		template<typename V>
		static V VisitVal(const PBEnum& Val)
		{
			return V(Val.EnumValue);
		}
		template<typename V, typename T>
		static std::enable_if_t<std::is_arithmetic<T>::value, V> VisitVal(const T& Val)
		{
			return V(Val);
		}
	};

	struct FProtoWriter : public FProtoReader
	{
		FDynamicArena Arena;
		FProtoWriter(upb_Message* InMsg, FFieldDefPtr InField, upb_Arena* InArena = nullptr)
			: FProtoReader(InMsg, InField)
			, Arena(InArena)
		{
		}
		FProtoWriter(FProtoReader& Ref, upb_Arena* InArena = nullptr)
			: FProtoReader(Ref)
			, Arena(InArena)
		{
		}
		FProtoWriter(FFieldDefPtr InField, upb_Arena* InArena = nullptr)
			: FProtoReader(AsVariant(InField.DefaultValue(), InField), InField)
			, Arena(InArena)
		{
		}
		upb_Arena* GetArena() { return *Arena; }

		upb_Message* MutableMsg() const { return std::get<upb_Message*>(Var); }
		upb_Array* MutableArr() const { return std::get<upb_Array*>(Var); }
		upb_Map* MutableMap() const { return std::get<upb_Map*>(Var); }

		template<typename T>
		bool SetFieldNum(const T& In)
		{
			if (!IsContainer())
			{
				Var = In;
				return true;
			}
			if (ensureAlways(TBaseFieldInfo<T>::EqualType(FieldDef.GetCType())))
			{
				if (FieldDef.GetArrayIdx() < 0)
				{
					_upb_Message_SetNonExtensionField(MutableMsg(), FieldDef.MiniTable(), &In);
					return true;
				}
				else if (ensureAlways(FieldDef.GetArrayIdx() < ArraySize()))
				{
					*(T*)ArrayElmData() = In;
					return true;
				}
			}
			return false;
		}

		template<typename T>
		bool SetFieldStr(const T& In)
		{
			if (!IsContainer())
			{
				Var = AllocStrView(In);
				return true;
			}
			if (ensureAlways(IsString()))
			{
				if (FieldDef.GetArrayIdx() < 0)
				{
					return upb_Message_SetString(MutableMsg(), FieldDef.MiniTable(), AllocStrView(In), Arena);
				}
				else if (ensureAlways(FieldDef.GetArrayIdx() < ArraySize()))
				{
					*(upb_StringView*)ArrayElmData() = AllocStrView(In);
					return true;
				}
			}
			return false;
		}

		template<typename T>
		bool SetFieldBytes(const T& In)
		{
			if (!IsContainer())
			{
				Var = AllocStrView(In);
				return true;
			}
			if (ensureAlways(IsBytes()))
			{
				if (FieldDef.GetArrayIdx() < 0)
				{
					return upb_Message_SetString(MutableMsg(), FieldDef.MiniTable(), AllocStrView(In), Arena);
				}
				else
				{
					*(upb_StringView*)ArrayElmData() = AllocStrView(In);
					return true;
				}
			}
			return false;
		}

		bool SetFieldMessage(upb_Message* SubMsgRef)
		{
			if (ensureAlways(IsMessage()))
			{
				if (FieldDef.GetArrayIdx() < 0)
				{
					upb_Message_SetMessage(MutableMsg(), MiniTable(), FieldDef.MiniTable(), SubMsgRef);
					return true;
				}
				else
				{
					*(upb_Message**)ArrayElmData() = SubMsgRef;
					return true;
				}
			}
			return false;
		}

		FProtoWriter ArrayElm(size_t Idx)
		{
			ArrayElmData(Idx);
			return FProtoWriter(MutableMsg(), FieldDef.GetElementDef(Idx));
		}

		bool InsertFieldMapEntry(upb_Message* EntryMsgRef)
		{
			if (ensureAlways(IsMap()))
			{
				upb_Message_InsertMapEntry(MutableMap(), FieldDef.MessageSubdef().MiniTable(), FieldDef.MiniTable(), EntryMsgRef, Arena);
				return true;
			}
			return false;
		}
		bool InsertFieldMapPair(FMessageVariant InKey, FMessageVariant InVal)
		{
			upb_MessageValue Key;
			upb_MessageValue Val;
			if (ensureAlways(IsMap() && FromVariant(Key, FieldDef.MapEntrySubdef().MapKeyDef(), InKey)  //
							 && FromVariant(Val, FieldDef.MapEntrySubdef().MapValueDef(), InVal)))
			{
				upb_MapInsertStatus Status = upb_Map_Insert(MutableMap(), Key, Val, Arena);
				return true;
			}
			return false;
		}

	protected:
		template<typename T>
		upb_StringView AllocStrView(const T& In)
		{
			if constexpr (std::is_same_v<T, upb_StringView> || std::is_same_v<T, StringView>)
			{
				return In;
			}
			else
			{
				return upb_StringView(StringView(In, *Arena));
			}
		}
		upb_Map* MutableMap()
		{
			GMP_CHECK(IsMap());
			return upb_Message_GetOrCreateMutableMap(MutableMsg(), FieldDef.MessageSubdef().MiniTable(), FieldDef.MiniTable(), Arena);
		}
		void* ArrayElmData(size_t Idx)
		{
			GMP_CHECK(IsArray());
			return upb_Message_ResizeArrayUninitialized(MutableMsg(), FieldDef.MiniTable(), Idx, Arena);
		}
		void* ArrayElmData()
		{
			GMP_CHECK(FieldDef.GetArrayIdx() >= 0);
			return ArrayElmData(FieldDef.GetArrayIdx());
		}
		FProtoWriter(FProtoWriter& Ref, int32_t Idx)
			: FProtoReader(Ref.GetMsg(), Ref.FieldDef.GetElementDef(Idx))
			, Arena(*Ref.Arena)
		{
		}
	};

	namespace Serializer
	{
		uint32 PropToField(FProtoWriter& Value, FProperty* Prop, const void* Addr);
		uint32 PropToField(FMessageDefPtr& MsgDef, FStructProperty* StructProp, const void* StructAddr, upb_Arena* Arena, upb_Message* MsgPtr = nullptr)
		{
			auto MsgRef = MsgPtr ? MsgPtr : upb_Message_New(MsgDef.MiniTable(), Arena);

			uint32 Ret = 0;
			for (FFieldDefPtr FieldDef : MsgDef.Fields())
			{
				auto Prop = StructProp->Struct->FindPropertyByName(FieldDef.Name().ToFName());
				if (Prop)
				{
					FProtoWriter ValRef(MsgRef, FieldDef, Arena);
					Ret += PropToField(ValRef, Prop, Prop->ContainerPtrToValuePtr<void>(StructAddr));
				}
				else
				{
					UE_LOG(LogGMP, Warning, TEXT("Field %s not found in struct %s"), *FieldDef.Name().ToFStringData(), *StructProp->GetName());
				}
			}
			return Ret;
		}

		bool UStructToProtoImpl(const UScriptStruct* Struct, const void* StructAddr, char** OutBuf, size_t* OutSize, FArena& Arena)
		{
			if (auto MsgDef = FindMessageByStruct(Struct))
			{
				auto MsgRef = upb_Message_New(MsgDef.MiniTable(), Arena);
				auto Ret = PropToField(MsgDef, GMP::Class2Prop::TTraitsStructBase::GetProperty(Struct), StructAddr, Arena, MsgRef);
				upb_EncodeStatus Status = upb_Encode(MsgRef, MsgDef.MiniTable(), 0, Arena, OutBuf, OutSize);
				if (!ensureAlways(Status == upb_EncodeStatus::kUpb_EncodeStatus_Ok))
					return false;
			}
			else
			{
				UE_LOG(LogGMP, Warning, TEXT("Message %s not found"), *Struct->GetName());
				return false;
			}
			return true;
		}
		bool UStructToProtoImpl(FArchive& Ar, const UScriptStruct* Struct, const void* StructAddr)
		{
			FArena Arena;
			char* OutBuf = nullptr;
			size_t OutSize = 0;
			auto Ret = UStructToProtoImpl(Struct, StructAddr, &OutBuf, &OutSize, Arena);
			if (OutSize && OutBuf)
			{
				Ar.Serialize(OutBuf, OutSize);
			}
			return Ret;
		}
		bool UStructToProtoImpl(TArray<uint8>& Out, const UScriptStruct* Struct, const void* StructAddr)
		{
			TMemoryWriter<32> Writer(Out);
			return UStructToProtoImpl(Writer, Struct, StructAddr);
		}
	}  // namespace Serializer

#if WITH_GMPVALUE_ONEOF
	static auto FriendGMPValueOneOf = [](const FGMPValueOneOf& In) -> decltype(auto) {
		struct FGMPValueOneOfFriend : public FGMPValueOneOf
		{
			using FGMPValueOneOf::Value;
			using FGMPValueOneOf::Flags;
		};
		return const_cast<FGMPValueOneOfFriend&>(static_cast<const FGMPValueOneOfFriend&>(In));
	};
	struct FPBValueHolder
	{
		FProtoReader Reader;
		FDynamicArena Arena;
		FPBValueHolder(const upb_Message* InMsg, FFieldDefPtr InField, upb_Arena* InArena = nullptr)
			: Reader(InMsg, InField)
			, Arena(InArena)
		{
		}
	};
#endif

	namespace Deserializer
	{
		uint32 FieldToProp(const FProtoReader& InVal, FProperty* Prop, void* Addr);
		uint32 FieldToProp(const FMessageDefPtr& MsgDef, const upb_Message* MsgRef, FStructProperty* StructProp, void* StructAddr)
		{
			uint32 Ret = 0;
			for (FFieldDefPtr FieldDef : MsgDef.Fields())
			{
				auto Prop = StructProp->Struct->FindPropertyByName(FieldDef.Name().ToFName());
				if (Prop)
				{
					Ret += FieldToProp(FProtoReader(MsgRef, FieldDef), Prop, Prop->ContainerPtrToValuePtr<void>(StructAddr));
				}
				else
				{
					UE_LOG(LogGMP, Warning, TEXT("Field %s not found in struct %s"), *FieldDef.Name().ToFStringData(), *StructProp->GetName());
				}
			}
			return Ret;
		}

		bool UStructFromProtoImpl(TArrayView<const uint8> In, const UScriptStruct* Struct, void* StructAddr)
		{
			if (auto MsgDef = FindMessageByStruct(Struct))
			{
				FDynamicArena Arena;
				upb_Message* MsgRef = upb_Message_New(MsgDef.MiniTable(), Arena);
				upb_DecodeStatus Status = upb_Decode((const char*)In.GetData(), In.Num(), MsgRef, MsgDef.MiniTable(), nullptr, 0, Arena);
				if (!ensureAlways(Status == upb_DecodeStatus::kUpb_DecodeStatus_Ok))
					return false;
				if (!FieldToProp(MsgDef, MsgRef, GMP::Class2Prop::TTraitsStructBase::GetProperty(Struct), StructAddr))
					return false;
			}
			else
			{
				UE_LOG(LogGMP, Warning, TEXT("Message %s not found"), *Struct->GetName());
				return false;
			}
			return true;
		}
		bool UStructFromProtoImpl(FArchive& Ar, const UScriptStruct* Struct, void* StructAddr)
		{
			TArray64<uint8> Buf;
			Buf.AddUninitialized(Ar.TotalSize());
			Ar.Serialize(Buf.GetData(), Buf.Num());
			return UStructFromProtoImpl(Buf, Struct, StructAddr);
		}
	}  // namespace Deserializer

	namespace Detail
	{

		template<typename WriterType>
		bool WriteToPB(WriterType& Writer, FProperty* Prop, const void* Value);
		template<typename ReaderType>
		bool ReadFromPB(const ReaderType& Val, FProperty* Prop, void* Value);

		using StringView = upb::StringView;
		namespace Internal
		{
			//////////////////////////////////////////////////////////////////////////
			struct FValueVisitorBase
			{
				static FString ExportText(FProperty* Prop, const void* Value)
				{
					FString StringValue;
#if UE_5_02_OR_LATER
					Prop->ExportTextItem_Direct(StringValue, Value, NULL, NULL, PPF_None);
#else
					Prop->ExportTextItem(StringValue, Value, NULL, NULL, PPF_None);
#endif
					return StringValue;
				}
				static void ImportText(const TCHAR* Str, FProperty* Prop, void* Addr, int32 ArrIdx)
				{
#if UE_5_02_OR_LATER
					Prop->ImportText_Direct(Str, Prop->template ContainerPtrToValuePtr<void>(Addr, ArrIdx), nullptr, PPF_None);
#else
					Prop->ImportText(Str, Prop->template ContainerPtrToValuePtr<void>(Addr, ArrIdx), PPF_None, nullptr);
#endif
				}
				static bool CanHoldWithDouble(uint64 u)
				{
					volatile double d = static_cast<double>(u);
					return (d >= 0.0) && (d < static_cast<double>((std::numeric_limits<uint64>::max)())) && (u == static_cast<uint64>(d));
				}
				static bool CanHoldWithDouble(int64 i)
				{
					volatile double d = static_cast<double>(i);
					return (d >= static_cast<double>((std::numeric_limits<int64>::min)())) && (d < static_cast<double>((std::numeric_limits<int64>::max)())) && (i == static_cast<int64>(d));
				}
				template<typename WriterType>
				static FORCEINLINE void WriteVisitStr(WriterType& Writer, const FString& Str)
				{
					Writer.SetFieldStr(Str);
				}
			};

			template<typename P>
			struct TValueVisitorDefault : public FValueVisitorBase
			{
				template<typename WriterType>
				static void WriteVisit(WriterType& Writer, P* Prop, const void* Addr, int32 ArrIdx)
				{
				}

				static FORCEINLINE void ReadVisit(const StringView& Val, P* Prop, void* Addr, int32 ArrIdx) {}
				static FORCEINLINE void ReadVisit(const std::monostate& Val, P* Prop, void* Addr, int32 ArrIdx) {}
				template<typename T>
				static FORCEINLINE std::enable_if_t<std::is_arithmetic<T>::value> ReadVisit(T Val, P* Prop, void* Addr, int32 ArrIdx)
				{
				}
				template<typename ReaderType>
				static FORCEINLINE void ReadVisit(const ReaderType* Ptr, P* Prop, void* Addr, int32 ArrIdx)
				{
				}
			};

			template<typename P>
			struct TValueVisitor : public TValueVisitorDefault<FProperty>
			{
				using TValueVisitorDefault<FProperty>::ReadVisit;
			};

			template<>
			struct TValueVisitor<FBoolProperty> : public TValueVisitorDefault<FBoolProperty>
			{
				template<typename WriterType>
				static void WriteVisit(WriterType& Writer, FBoolProperty* Prop, const void* Addr, int32 ArrIdx)
				{
					auto Value = Prop->template ContainerPtrToValuePtr<void>(Addr, ArrIdx);
					bool BoolVal = Prop->GetPropertyValue(Value);
					Writer.SetFieldNum(BoolVal);
				}
				using TValueVisitorDefault<FBoolProperty>::ReadVisit;
				template<typename T>
				static std::enable_if_t<std::is_arithmetic<T>::value> ReadVisit(T Val, FBoolProperty* Prop, void* Addr, int32 ArrIdx)
				{
					ensure(Val == 0 || Val == 1);
					Prop->SetPropertyValue(Prop->template ContainerPtrToValuePtr<void>(Addr, ArrIdx), !!Val);
				}

				static void ReadVisit(const StringView& Val, FBoolProperty* Prop, void* Addr, int32 ArrIdx) { Prop->SetPropertyValue(Prop->template ContainerPtrToValuePtr<void>(Addr, ArrIdx), FCStringAnsi::ToBool(Val)); }
			};
			template<>
			struct TValueVisitor<FEnumProperty> : public TValueVisitorDefault<FEnumProperty>
			{
				template<typename WriterType>
				static void WriteVisit(WriterType& Writer, FEnumProperty* Prop, const void* Addr, int32 ArrIdx)
				{
					auto Value = Prop->template ContainerPtrToValuePtr<void>(Addr, ArrIdx);
					auto IntVal = Prop->GetUnderlyingProperty()->GetSignedIntPropertyValue(Value);
					Writer.SetFieldNum((int32)IntVal);
				}

				using TValueVisitorDefault<FEnumProperty>::ReadVisit;
				template<typename T>
				static std::enable_if_t<std::is_arithmetic<T>::value> ReadVisit(T Val, FEnumProperty* Prop, void* Addr, int32 ArrIdx)
				{
					Prop->GetUnderlyingProperty()->SetIntPropertyValue(Prop->template ContainerPtrToValuePtr<void>(Addr, ArrIdx), (int64)Val);
				}

				static void ReadVisit(const StringView& Val, FEnumProperty* Prop, void* Addr, int32 ArrIdx)
				{
					const UEnum* Enum = Prop->GetEnum();
					check(Enum);
					int64 IntValue = Enum->GetValueByNameString(Val);
					Prop->GetUnderlyingProperty()->SetIntPropertyValue(Prop->template ContainerPtrToValuePtr<void>(Addr, ArrIdx), IntValue);
				}
			};
			template<>
			struct TValueVisitor<FNumericProperty> : public TValueVisitorDefault<FNumericProperty>
			{
				template<typename WriterType>
				static void WriteVisit(WriterType& Writer, FNumericProperty* Prop, const void* Addr, int32 ArrIdx)
				{
					auto Value = Prop->template ContainerPtrToValuePtr<void>(Addr, ArrIdx);
					if (UEnum* EnumDef = Prop->GetIntPropertyEnum())
					{
						auto IntVal = Prop->GetSignedIntPropertyValue(Value);
						Writer.SetFieldNum((int32)IntVal);
					}
					else if (Prop->IsFloatingPoint())
					{
						const bool bIsDouble = Prop->IsA<FDoubleProperty>();
						if (bIsDouble)
						{
							double d = CastFieldChecked<FDoubleProperty>(Prop)->GetPropertyValue(Value);
							Writer.SetFieldNum(d);
						}
						else
						{
							float f = CastFieldChecked<FFloatProperty>(Prop)->GetPropertyValue(Value);
							Writer.SetFieldNum(f);
						}
					}
					else if (Prop->IsA<FUInt64Property>())
					{
						uint64 UIntVal = Prop->GetUnsignedIntPropertyValue(Value);
						Writer.SetFieldNum(UIntVal);
					}
					else if (Prop->IsA<FInt64Property>())
					{
						int64 IntVal = Prop->GetSignedIntPropertyValue(Value);
						Writer.SetFieldNum(IntVal);
					}
					else if (Prop->IsA<FIntProperty>())
					{
						int32 IntVal = Prop->GetSignedIntPropertyValue(Value);
						Writer.SetFieldNum(IntVal);
					}
					else if (Prop->IsA<FUInt32Property>())
					{
						uint32 IntVal = Prop->GetUnsignedIntPropertyValue(Value);
						Writer.SetFieldNum(IntVal);
					}
					else
					{
						ensureAlways(false);
					}
				}

				using TValueVisitorDefault<FNumericProperty>::ReadVisit;
				template<typename T>
				static std::enable_if_t<std::is_arithmetic<T>::value> ReadVisit(T Val, FNumericProperty* Prop, void* Addr, int32 ArrIdx)
				{
					auto Value = Prop->template ContainerPtrToValuePtr<void>(Addr, ArrIdx);
					if (Prop->IsFloatingPoint())
					{
						if (auto FloatProp = CastField<FFloatProperty>(Prop))
							FloatProp->SetPropertyValue(Value, (float)Val);
						else
							Prop->SetFloatingPointPropertyValue(Value, (double)Val);
					}
					else
					{
						Prop->SetIntPropertyValue(Value, (int64)Val);
					}
				}

				static void ReadVisit(const StringView& Val, FNumericProperty* Prop, void* Addr, int32 ArrIdx)
				{
					auto Value = Prop->template ContainerPtrToValuePtr<void>(Addr, ArrIdx);
					if (UEnum* EnumDef = Prop->GetIntPropertyEnum())
					{
						auto EnumVal = EnumDef->GetValueByNameString(Val);
						Prop->SetIntPropertyValue(Value, EnumVal);
					}
					else
					{
						Prop->SetNumericPropertyValueFromString(Value, Val.ToFStringData());
					}
				}
			};

			template<typename P>
			struct TNumericValueVisitor
			{
				using NumericType = typename P::TCppType;
				template<typename WriterType>
				static void WriteVisit(WriterType& Writer, P* Prop, const void* Addr, int32 ArrIdx)
				{
					auto Value = Prop->template ContainerPtrToValuePtr<NumericType>(Addr, ArrIdx);
					auto Val = Prop->GetPropertyValue(Value);
					using TargetType = std::conditional_t<sizeof(NumericType) < sizeof(int32), int32, NumericType>;
					Writer.SetFieldNum((TargetType)Val);
				}

				template<typename T>
				static std::enable_if_t<std::is_arithmetic<T>::value> ReadVisit(T Val, P* Prop, void* Addr, int32 ArrIdx)
				{
					auto Value = Prop->template ContainerPtrToValuePtr<NumericType>(Addr, ArrIdx);
					Prop->SetPropertyValue(Value, Val);
				}
				static void ReadVisit(const StringView& Val, P* Prop, void* Addr, int32 ArrIdx)
				{
					auto* ValuePtr = Prop->template ContainerPtrToValuePtr<NumericType>(Addr, ArrIdx);
					LexFromString(*ValuePtr, Val.ToFStringData());
				}
				static FORCEINLINE void ReadVisit(const std::monostate& Val, P* Prop, void* Addr, int32 ArrIdx) {}
				template<typename ReaderType>
				static FORCEINLINE void ReadVisit(const ReaderType* Ptr, P* Prop, void* Addr, int32 ArrIdx)
				{
				}
			};
			template<>
			struct TValueVisitor<FFloatProperty> : public TNumericValueVisitor<FFloatProperty>
			{
			};
			template<>
			struct TValueVisitor<FDoubleProperty> : public TNumericValueVisitor<FDoubleProperty>
			{
			};
			template<>
			struct TValueVisitor<FInt8Property> : public TNumericValueVisitor<FInt8Property>
			{
			};
			template<>
			struct TValueVisitor<FInt16Property> : public TNumericValueVisitor<FInt16Property>
			{
			};
			template<>
			struct TValueVisitor<FIntProperty> : public TNumericValueVisitor<FIntProperty>
			{
			};
			template<>
			struct TValueVisitor<FInt64Property> : public TNumericValueVisitor<FInt64Property>
			{
			};
			template<>
			struct TValueVisitor<FUInt16Property> : public TNumericValueVisitor<FUInt16Property>
			{
			};
			template<>
			struct TValueVisitor<FUInt32Property> : public TNumericValueVisitor<FUInt32Property>
			{
			};
			template<>
			struct TValueVisitor<FUInt64Property> : public TNumericValueVisitor<FUInt64Property>
			{
			};
			template<>
			struct TValueVisitor<FByteProperty>
			{
				template<typename WriterType>
				static void WriteVisit(WriterType& Writer, FByteProperty* Prop, const void* Addr, int32 ArrIdx)
				{
					auto Value = Prop->template ContainerPtrToValuePtr<void>(Addr, ArrIdx);
					auto Val = Prop->GetPropertyValue(Value);
					using TargetType = std::conditional_t<sizeof(Val) < sizeof(int32), int32, decltype(Val)>;
					Writer.SetFieldNum((TargetType)Val);
				}
				static FORCEINLINE void ReadVisit(const std::monostate& Val, FByteProperty* Prop, void* Addr, int32 ArrIdx) {}
				static FORCEINLINE void ReadVisit(bool bVal, FByteProperty* Prop, void* Addr, int32 ArrIdx)
				{
					auto Value = Prop->template ContainerPtrToValuePtr<void>(Addr, ArrIdx);
					uint8 Val = bVal ? 1 : 0;
					Prop->SetPropertyValue(Value, Val);
				}
				template<typename ReaderType>
				static FORCEINLINE void ReadVisit(const ReaderType* Ptr, FByteProperty* Prop, void* Addr, int32 ArrIdx)
				{
				}

				template<typename T>
				static std::enable_if_t<std::is_arithmetic<T>::value> ReadVisit(T Val, FByteProperty* Prop, void* Addr, int32 ArrIdx)
				{
					auto Value = Prop->template ContainerPtrToValuePtr<uint8>(Addr, ArrIdx);
					if (ensureAlways(Val >= 0 && Val <= (std::numeric_limits<uint8>::max)()))
						Prop->SetPropertyValue(Value, (uint8)Val);
				}

				static void ReadVisit(const StringView& Val, FByteProperty* Prop, void* Addr, int32 ArrIdx)
				{
					auto ValuePtr = Prop->template ContainerPtrToValuePtr<uint8>(Addr, ArrIdx);
					if (UEnum* EnumDef = Prop->GetIntPropertyEnum())  // TEnumAsByte
					{
						auto EnumVal = EnumDef->GetValueByNameString(Val);
						if (ensureAlways(EnumVal >= 0 && EnumVal <= (std::numeric_limits<uint8>::max)()))
							Prop->SetPropertyValue(ValuePtr, (uint8)EnumVal);
					}
					else
					{
						LexFromString(*ValuePtr, Val.ToFStringData());
					}
				}
			};
			template<>
			struct TValueVisitor<FStrProperty> : public TValueVisitorDefault<FStrProperty>
			{
				template<typename WriterType>
				static void WriteVisit(WriterType& Writer, FStrProperty* Prop, const void* Addr, int32 ArrIdx)
				{
					FValueVisitorBase::WriteVisitStr(Writer, Prop->GetPropertyValue_InContainer(Addr, ArrIdx));
				}

				using TValueVisitorDefault<FStrProperty>::ReadVisit;
				template<typename T>
				static std::enable_if_t<std::is_arithmetic<T>::value> ReadVisit(T Val, FStrProperty* Prop, void* Addr, int32 ArrIdx)
				{
					FString* Str = Prop->template ContainerPtrToValuePtr<FString>(Addr, ArrIdx);
					*Str = LexToString(Val);
				}
				static void ReadVisit(const StringView& Val, FStrProperty* Prop, void* Addr, int32 ArrIdx)
				{
					FString* Str = Prop->template ContainerPtrToValuePtr<FString>(Addr, ArrIdx);
					*Str = Val.ToFString();
				}
			};
			template<>
			struct TValueVisitor<FNameProperty> : public TValueVisitorDefault<FNameProperty>
			{
				template<typename WriterType>
				static void WriteVisit(WriterType& Writer, FNameProperty* Prop, const void* Addr, int32 ArrIdx)
				{
					FName Name = Prop->GetPropertyValue_InContainer(Addr, ArrIdx);
					FValueVisitorBase::WriteVisitStr(Writer, Name.ToString());
				}

				using TValueVisitorDefault<FNameProperty>::ReadVisit;
				static void ReadVisit(const StringView& Val, FNameProperty* Prop, void* Addr, int32 ArrIdx)
				{
					FName* Name = Prop->template ContainerPtrToValuePtr<FName>(Addr, ArrIdx);
					*Name = Val.ToFName(FNAME_Add);
				}
			};
			template<>
			struct TValueVisitor<FTextProperty> : public TValueVisitorDefault<FTextProperty>
			{
				template<typename WriterType>
				static void WriteVisit(WriterType& Writer, FTextProperty* Prop, const void* Addr, int32 ArrIdx)
				{
					FText Text = Prop->GetPropertyValue_InContainer(Addr, ArrIdx);
					FValueVisitorBase::WriteVisitStr(Writer, Text.ToString());
				}

				using TValueVisitorDefault<FTextProperty>::ReadVisit;
				static void ReadVisit(const StringView& Val, FTextProperty* Prop, void* Addr, int32 ArrIdx)
				{
					FText* Text = Prop->template ContainerPtrToValuePtr<FText>(Addr, ArrIdx);
					// FValueVisitorBase::ImportText(Val.ToFStringData(), Prop, Addr, ArrIdx);
					*Text = FText::FromString(Val);
				}

#if 0
				template<typename ReaderType>
				static void ReadVisit(const ReaderType* Ptr, FTextProperty* Prop, void* Addr, int32 ArrIdx)
				{
					FText* Text = Prop->template ContainerPtrToValuePtr<FText>(Addr, ArrIdx);
					*Text = FText::FromString(Ptr->GetFieldStr<FString>());
				}
#endif
			};

			template<>
			struct TValueVisitor<FSoftObjectProperty> : public TValueVisitorDefault<FSoftObjectProperty>
			{
				template<typename WriterType>
				static void WriteVisit(WriterType& Writer, FSoftObjectProperty* Prop, const void* Addr, int32 ArrIdx)
				{
					auto Value = Prop->template ContainerPtrToValuePtr<void>(Addr, ArrIdx);
					UObject* Obj = Prop->GetObjectPropertyValue(Value);
					FValueVisitorBase::WriteVisitStr(Writer, GIsEditor ? GetPathNameSafe(Obj) : GetPathNameSafe(Obj));
				}
				using TValueVisitorDefault<FSoftObjectProperty>::ReadVisit;
				static void ReadVisit(const StringView& Val, FSoftObjectProperty* Prop, void* Addr, int32 ArrIdx)
				{
					FSoftObjectPath* OutValue = (FSoftObjectPath*)Prop->template ContainerPtrToValuePtr<void>(Addr, ArrIdx);
					if (GIsEditor && GWorld)
					{
#if UE_5_02_OR_LATER
						OutValue->SetPath(UWorld::ConvertToPIEPackageName(Val, GWorld->GetPackage()->GetPIEInstanceID()));
#else
						OutValue->SetPath(UWorld::ConvertToPIEPackageName(Val, GWorld->GetPackage()->PIEInstanceID));
#endif
					}
					else
					{
						OutValue->SetPath(Val.ToFStringData());
					}
				}
			};

			template<>
			struct TValueVisitor<FStructProperty> : public TValueVisitorDefault<FStructProperty>
			{
				static uint32 StructToMessage(FProtoWriter& Writer, FStructProperty* StructProp, const void* StructAddr)
				{
					uint32 Ret = 0;
					if (ensureAlways(Writer.IsMessage()))
					{
#if WITH_GMPVALUE_ONEOF
						if (StructProp->Struct == FGMPValueOneOf::StaticStruct())
						{
							auto OneOf = (FGMPValueOneOf*)StructAddr;
							auto OneOfPtr = &FriendGMPValueOneOf(*OneOf);
							if (ensure(OneOf->IsValid()))
							{
								auto Ptr = StaticCastSharedPtr<FPBValueHolder>(OneOfPtr->Value);
								auto SubMsgDef = Ptr->Reader.FieldDef.MessageSubdef();
								auto SubMsgRef = upb_Message_New(SubMsgDef.MiniTable(), Ptr->Arena);
								Ret = Serializer::PropToField(SubMsgDef, StructProp, StructAddr, Writer.GetArena(), SubMsgRef);
								Writer.SetFieldMessage(SubMsgRef);
							}
						}
						else
#endif

						{
							auto SubMsgDef = Writer.FieldDef.MessageSubdef();
							auto SubMsgRef = upb_Message_New(SubMsgDef.MiniTable(), Writer.GetArena());
							Ret = Serializer::PropToField(SubMsgDef, StructProp, StructAddr, Writer.GetArena(), SubMsgRef);
							Writer.SetFieldMessage(SubMsgRef);
						}
					}
					return Ret;
				}
				static uint32 MessageToStruct(const FProtoReader& Reader, FStructProperty* StructProp, void* StructAddr)
				{
					uint32 Ret = 0;
					if (ensureAlways(Reader.IsMessage()))
					{
						auto MsgDef = Reader.FieldDef.MessageSubdef();
						auto MsgRef = Reader.GetSubMessage();
#if WITH_GMPVALUE_ONEOF
						if (StructProp->Struct == FGMPValueOneOf::StaticStruct())
						{
							auto Ref = MakeShared<FPBValueHolder>(nullptr, Reader.FieldDef);
							Ref->Reader.Var = upb_Message_DeepClone(MsgRef, MsgDef.MiniTable(), Ref->Arena);
							auto OneOf = (FGMPValueOneOf*)StructAddr;
							auto& Holder = FriendGMPValueOneOf(*OneOf);
							Holder.Value = MoveTemp(Ref);
							Holder.Flags = 0;
							Ret = 1;
						}
						else
#endif
						{
							Ret = Deserializer::FieldToProp(MsgDef, MsgRef, StructProp, StructAddr);
						}
					}
					return Ret;
				}

				template<typename WriterType>
				static void WriteVisit(WriterType& Writer, FStructProperty* Prop, const void* Addr, int32 ArrIdx)
				{
					auto Value = Prop->template ContainerPtrToValuePtr<void>(Addr, ArrIdx);
					StructToMessage(Writer, Prop, Addr);
				}

				using TValueVisitorDefault<FStructProperty>::ReadVisit;
				static void ReadVisit(const StringView& Val, FStructProperty* Prop, void* Addr, int32 ArrIdx) { FValueVisitorBase::ImportText(Val.ToFStringData(), Prop, Addr, ArrIdx); }

				template<typename ReaderType>
				static void ReadVisit(const ReaderType* Ptr, FStructProperty* Prop, void* Addr, int32 ArrIdx)
				{
					auto& Reader = *Ptr;
					auto OutValue = Prop->template ContainerPtrToValuePtr<void>(Addr, ArrIdx);
					MessageToStruct(Reader, Prop, Addr);
				}
			};

			template<>
			struct TValueVisitor<FArrayProperty> : public TValueVisitorDefault<FArrayProperty>
			{
				template<typename WriterType>
				static void WriteVisit(WriterType& Writer, FArrayProperty* Prop, const void* Addr, int32 ArrIdx)
				{
					ensureAlways(Prop->ArrayDim == 1 && ArrIdx == 0);
					auto Value = Prop->template ContainerPtrToValuePtr<void>(Addr, 0);

					// Bytes
					if (Writer.IsBytes() && ensureAlways(Prop->Inner->IsA<FByteProperty>() || Prop->Inner->IsA<FInt8Property>()))
					{
						FScriptArrayHelper Helper(Prop, Value);
						Writer.SetFieldBytes(StringView((const char*)Helper.GetRawPtr(), Helper.Num()));
					}
					else if (ensure(Writer.IsArray()))
					{
						FScriptArrayHelper Helper(Prop, Value);
						for (int32 i = 0; i < Helper.Num(); ++i)
						{
							auto Elm = Writer.ArrayElm(i);
							WriteToPB(Elm, Prop->Inner, Helper.GetRawPtr(i));
						}
					}
					else
					{
						FScriptArrayHelper Helper(Prop, Value);
						if (Helper.Num() > 0)
							WriteToPB(Writer, Prop->Inner, Helper.GetRawPtr(0));
					}
				}
				using TValueVisitorDefault<FArrayProperty>::ReadVisit;
				template<typename ReaderType>
				static void ReadVisit(const ReaderType* Ptr, FArrayProperty* Prop, void* Addr, int32 ArrIdx)
				{
					auto& Reader = *Ptr;
					ensureAlways(Prop->ArrayDim == 1 && ArrIdx == 0);
					auto OutValue = Prop->template ContainerPtrToValuePtr<void>(Addr, 0);

					// Bytes
					if (Reader.IsBytes() && ensureAlways(Prop->IsA<FByteProperty>() || Prop->IsA<FInt8Property>()))
					{
						auto View = StringView(Reader.GetFieldBytes());
						FScriptArrayHelper Helper(Prop, OutValue);
						Helper.Resize(View.size());
						FMemory::Memcpy(Helper.GetRawPtr(), View.data(), View.size());
					}
					else if (ensure(Reader.IsArray()))
					{
						auto ItemsToRead = FMath::Max((int32)Reader.ArraySize(), 0);
						FScriptArrayHelper Helper(Prop, OutValue);
						Helper.Resize(ItemsToRead);
						for (auto i = 0; i < Helper.Num(); ++i)
						{
							ReadFromPB(Reader.ArrayElm(i), Prop->Inner, Helper.GetRawPtr(i));
						}
					}
					else
					{
						FScriptArrayHelper Helper(Prop, OutValue);
						Helper.Resize(1);
						ReadFromPB(Reader, Prop->Inner, Helper.GetRawPtr(0));
					};
				}
			};
			template<>
			struct TValueVisitor<FSetProperty> : public TValueVisitorDefault<FSetProperty>
			{
				template<typename WriterType>
				static void WriteVisit(WriterType& Writer, FSetProperty* Prop, const void* Addr, int32 ArrIdx)
				{
					ensureAlways(Prop->ArrayDim == 1 && ArrIdx == 0);
					auto Value = Prop->template ContainerPtrToValuePtr<void>(Addr, 0);
					if (ensure(Writer.IsArray()))
					{
						FScriptSetHelper Helper(Prop, Value);
						for (int32 i = 0; i < Helper.Num(); ++i)
						{
							if (Helper.IsValidIndex(i))
							{
								auto Elm = Writer.ArrayElm(i);
								WriteToPB(Elm, Prop->ElementProp, Helper.GetElementPtr(i));
							}
						}
					}
				}
				using TValueVisitorDefault<FSetProperty>::ReadVisit;
				template<typename ReaderType>
				static void ReadVisit(const ReaderType* Ptr, FSetProperty* Prop, void* Addr, int32 ArrIdx)
				{
					auto& Reader = *Ptr;
					ensureAlways(Prop->ArrayDim == 1 && ArrIdx == 0);
					auto OutValue = Prop->template ContainerPtrToValuePtr<void>(Addr, 0);
					if (ensure(Reader.IsArray()))
					{
						FScriptSetHelper Helper(Prop, OutValue);
						for (auto i = 0; i < Reader.ArraySize(); ++i)
						{
							int32 NewIndex = Helper.AddDefaultValue_Invalid_NeedsRehash();
							ReadFromPB(Reader.ArrayElm(i), Prop->ElementProp, Helper.GetElementPtr(NewIndex));
						}
						Helper.Rehash();
					}
					else
					{
						FScriptSetHelper Helper(Prop, OutValue);
						Helper.EmptyElements(1);
						int32 NewIndex = Helper.AddDefaultValue_Invalid_NeedsRehash();
						ReadFromPB(Reader, Prop->ElementProp, Helper.GetElementPtr(NewIndex));
						Helper.Rehash();
					}
				}
			};
			template<>
			struct TValueVisitor<FMapProperty> : public TValueVisitorDefault<FMapProperty>
			{
				static uint32 PropToMap(FProtoWriter& Writer, FMapProperty* MapProp, const void* MapAddr)
				{
					uint32 Ret = 0;
					if (ensureAlways(Writer.IsMap()))
					{
						auto MapEntryDef = Writer.MapEntryDef();

						FScriptMapHelper Helper(MapProp, MapAddr);
						for (auto i = 0; i < Helper.Num(); ++i)
						{
							if (!Helper.IsValidIndex(i))
								continue;

							FProtoWriter KeyWriter(MapEntryDef.MapKeyDef(), Writer.GetArena());
							Ret = Serializer::PropToField(KeyWriter, MapProp->KeyProp, Helper.GetKeyPtr(i));
							FProtoWriter ValueWriter(MapEntryDef.MapValueDef(), Writer.GetArena());
							Ret = Serializer::PropToField(ValueWriter, MapProp->ValueProp, Helper.GetValuePtr(i));

							Writer.InsertFieldMapPair(KeyWriter.Var, ValueWriter.Var);
						}
					}
					return Ret;
				}
				template<typename WriterType>
				static void WriteVisit(WriterType& Writer, FMapProperty* Prop, const void* Addr, int32 ArrIdx)
				{
					if (!ensure(Writer.IsMap()))
						return;
					ensureAlways(Prop->ArrayDim == 1 && ArrIdx == 0);
					auto Value = Prop->template ContainerPtrToValuePtr<void>(Addr, 0);
					PropToMap(Writer, Prop, Value);
				}

				static uint32 MapToProp(const FProtoReader& Reader, FMapProperty* MapProp, void* MapAddr)
				{
					uint32 Ret = 0;
					if (ensureAlways(Reader.IsMap()))
					{
						auto MapRef = Reader.GetSubMap();
						auto MapSize = upb_Map_Size(MapRef);

						FScriptMapHelper Helper(MapProp, MapAddr);
						Helper.EmptyValues(MapSize);

						auto MapDef = Reader.MapEntryDef();
						size_t Iter = kUpb_Map_Begin;
						upb_MessageValue key;
						upb_MessageValue val;
						while (upb_Map_Next(MapRef, &key, &val, &Iter))
						{
							Helper.AddDefaultValue_Invalid_NeedsRehash();
							FProtoReader KeyReader(key, MapDef.MapKeyDef());
							Ret += Deserializer::FieldToProp(KeyReader, MapProp->KeyProp, Helper.GetKeyPtr(Iter));
							FProtoReader ValueReader(val, MapDef.MapValueDef());
							Ret += Deserializer::FieldToProp(ValueReader, MapProp->ValueProp, Helper.GetValuePtr(Iter));
						}
						Helper.Rehash();
					}
					return Ret;
				}

				using TValueVisitorDefault<FMapProperty>::ReadVisit;
				template<typename ReaderType>
				static void ReadVisit(ReaderType* Ptr, FMapProperty* Prop, void* Addr, int32 ArrIdx)
				{
					auto& PBVal = *Ptr;
					ensureAlways(Prop->ArrayDim == 1 && ArrIdx == 0);
					auto OutValue = Prop->template ContainerPtrToValuePtr<void>(Addr, 0);
					MapToProp(PBVal, Prop, OutValue);
				}
			};

			template<typename P>
			struct TValueDispatcher
			{
				template<typename WriterType>
				static bool Write(WriterType& Writer, P* Prop, const void* Value)
				{
					if (ensureAlways(Prop->ArrayDim <= 1))
					{
						TValueVisitor<P>::WriteVisit(Writer, Prop, Value, 0);
					}
					return true;
				}

				template<typename ReaderType>
				static bool Read(const ReaderType& Val, P* Prop, void* Addr)
				{
					int32 i = 0;
					auto Visitor = [&](auto&& Elm) { TValueVisitor<P>::ReadVisit(std::forward<decltype(Elm)>(Elm), Prop, Addr, i); };
					if (Val.IsArray() && !CastField<FArrayProperty>(Prop) && !CastField<FSetProperty>(Prop))
					{
						int32 ItemsToRead = FMath::Clamp((int32)Val.ArraySize(), 0, Prop->ArrayDim);
						for (; i < ItemsToRead; ++i)
						{
							std::visit(Visitor, Val.ArrayElm(i).DispatchFieldValue());
						}
					}
					else
					{
						std::visit(Visitor, Val.DispatchFieldValue());
					}
					return true;
				}
			};
		}  // namespace Internal

		template<typename WriterType>
		bool WriteToPB(WriterType& Writer, FProperty* Prop, const void* Value)
		{
			return GMP::Serializer::Traits::ForeachProp([](auto& OutVal, auto* InProp, const void* InVal) -> bool { return Internal::TValueDispatcher<std::decay_t<decltype(*InProp)>>::Write(OutVal, InProp, InVal); }, Writer, Prop, Value);
		}
		template<typename ReaderType>
		bool ReadFromPB(const ReaderType& Reader, FProperty* Prop, void* Value)
		{
			return GMP::Serializer::Traits::ForeachProp([](const auto& InVal, auto* InProp, void* OutVal) -> bool { return Internal::TValueDispatcher<std::decay_t<decltype(*InProp)>>::Read(InVal, InProp, OutVal); }, Reader, Prop, Value);
		}
	}  // namespace Detail

	namespace Serializer
	{
		uint32 PropToField(FProtoWriter& Value, FProperty* Prop, const void* Addr)
		{
			uint32 Ret = 0;
			Detail::WriteToPB(Value, Prop, Addr);
			return Ret;
		}
	}  // namespace Serializer
	namespace Deserializer
	{
		uint32 FieldToProp(const FProtoReader& Value, FProperty* Prop, void* Addr)
		{
			uint32 Ret = 0;
			Detail::ReadFromPB(Value, Prop, Addr);
			return Ret;
		}
	}  // namespace Deserializer
}  // namespace PB
}  // namespace GMP

void ReigsterProtoDesc(const char* Buf, size_t Size)
{
	GMP::PB::AddProto(Buf, Size);
}

DEFINE_FUNCTION(UGMPProtoUtils::execAsStruct)
{
	P_GET_STRUCT_REF(FGMPValueOneOf, OneOf);

	Stack.MostRecentPropertyAddress = nullptr;
	Stack.MostRecentProperty = nullptr;
	Stack.StepCompiledIn<FProperty>(nullptr);
	void* OutData = Stack.MostRecentPropertyAddress;
	FProperty* OutProp = Stack.MostRecentProperty;
	P_GET_PROPERTY(FNameProperty, SubKey);
	P_GET_UBOOL(bConsumeOneOf);
	P_FINISH

	P_NATIVE_BEGIN
	*(bool*)RESULT_PARAM = AsValueImpl(OneOf, OutProp, OutData, SubKey);
	if (bConsumeOneOf)
		OneOf.Clear();
	P_NATIVE_END
}

DEFINE_FUNCTION(UGMPProtoUtils::execEncodeProto)
{
	Stack.MostRecentPropertyAddress = nullptr;
	Stack.MostRecentProperty = nullptr;
	Stack.StepCompiledIn<FStructProperty>(nullptr);
	const void* Data = Stack.MostRecentPropertyAddress;
	FStructProperty* Prop = CastField<FStructProperty>(Stack.MostRecentProperty);
	P_GET_TARRAY_REF(uint8, Buffer);
	P_FINISH

	P_NATIVE_BEGIN
#if defined(GMP_WITH_UPB)
	if (!Prop || !Prop->Struct->IsA<UProtoDefinedStruct>())
	{
		FFrame::KismetExecutionMessage(TEXT("invalid struct type"), ELogVerbosity::Error);
		*(bool*)RESULT_PARAM = false;
	}
	else
	{
		*(bool*)RESULT_PARAM = !!GMP::PB::Serializer::UStructToProtoImpl(Buffer, Prop->Struct, Data);
	}
#else
	*(bool*)RESULT_PARAM = false;
#endif
	P_NATIVE_END
}

DEFINE_FUNCTION(UGMPProtoUtils::execDecodeProto)
{
	P_GET_TARRAY_REF(uint8, Buffer);

	Stack.MostRecentPropertyAddress = nullptr;
	Stack.MostRecentProperty = nullptr;
	Stack.StepCompiledIn<FStructProperty>(nullptr);
	void* Data = Stack.MostRecentPropertyAddress;
	FStructProperty* Prop = CastField<FStructProperty>(Stack.MostRecentProperty);
	P_FINISH

#if defined(GMP_WITH_UPB)
	P_NATIVE_BEGIN
	if (!Prop || !Prop->Struct->IsA<UProtoDefinedStruct>())
	{
		FFrame::KismetExecutionMessage(TEXT("invalid struct type"), ELogVerbosity::Error);
		*(bool*)RESULT_PARAM = false;
	}
	else
	{
		*(bool*)RESULT_PARAM = !!GMP::PB::Deserializer::UStructFromProtoImpl(Buffer, Prop->Struct, Data);
	}
#else
	*(bool*)RESULT_PARAM = false;
#endif
	P_NATIVE_END
}

void UGMPProtoUtils::ClearOneOf(UPARAM(ref) FGMPValueOneOf& OneOf)
{
	OneOf.Clear();
}

bool UGMPProtoUtils::AsValueImpl(const FGMPValueOneOf& In, FProperty* Prop, void* Out, FName SubKey)
{
	using namespace GMP::PB;
	bool bRet = false;
	do
	{
		auto OneOfPtr = &FriendGMPValueOneOf(In);

		if (!OneOfPtr->IsValid())
			break;
#if WITH_GMPVALUE_ONEOF
		if (OneOfPtr->Flags == 0 && Prop->IsA<FStructProperty>())
		{
			auto Ptr = StaticCastSharedPtr<FPBValueHolder>(OneOfPtr->Value);
			const FProtoReader& Reader = Ptr->Reader;
			GMP::PB::Detail::Internal::TValueVisitor<FStructProperty>::ReadVisit(&Reader, CastFieldChecked<FStructProperty>(Prop), Out, 0);
			bRet = true;
		}
		else
		{
			ensure(false);
		}
#endif
	} while (false);
	return bRet;
}

int32 UGMPProtoUtils::IterateKeyValueImpl(const FGMPValueOneOf& In, int32 Idx, FString& OutKey, FGMPValueOneOf& OutValue)
{
	int32 RetIdx = INDEX_NONE;
	using namespace GMP::PB;
	do
	{
		auto OneOfPtr = &FriendGMPValueOneOf(In);

		if (!OneOfPtr->IsValid())
			break;

#if WITH_GMPVALUE_ONEOF
		if (OneOfPtr->Flags == 0)
		{
			auto Ptr = StaticCastSharedPtr<FPBValueHolder>(OneOfPtr->Value);
			const FProtoReader& Reader = Ptr->Reader;
			if (auto SubFieldDef = Reader.FieldDef.MessageSubdef().FindFieldByNumber(Idx))
			{
				Deserializer::FieldToProp(FProtoReader(Reader.GetSubMessage(), SubFieldDef), GMP::TClass2Prop<FGMPValueOneOf>::GetProperty(), &OutValue);
			}
		}
		else
		{
			ensure(false);
		}
#endif
	} while (false);
	return RetIdx;
}

#if WITH_EDITOR
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Developer/DesktopPlatform/Public/DesktopPlatformModule.h"
#include "Developer/DesktopPlatform/Public/IDesktopPlatform.h"
#include "EdGraphSchema_K2.h"
#include "Editor.h"
#include "GMPEditorUtils.h"
#include "Kismet2/EnumEditorUtils.h"
#include "Kismet2/StructureEditorUtils.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/ScopedSlowTask.h"
#include "ObjectTools.h"
#include "PackageTools.h"
#include "UserDefinedStructure/UserDefinedStructEditorData.h"
#include "XConsoleManager.h"

#if UE_VERSION_NEWER_THAN(5, 0, 0)
#include "UObject/ArchiveCookContext.h"
#include "UObject/SavePackage.h"
#endif

#if GMP_EXTEND_CONSOLE
namespace GMP
{
namespace PB
{
	class FProtoTraveler
	{
	protected:
		FString GetPackagePath(const FString& Sub) const
		{
			auto RetPath = FString::Printf(TEXT("%s/%s"), *RootPath, *Sub);
			FString FolderPath;
			FPackageName::TryConvertGameRelativePackagePathToLocalPath(RetPath, FolderPath);
			IPlatformFile::GetPlatformPhysical().CreateDirectoryTree(*FPaths::GetPath(FolderPath));
			return RetPath;
		}

		FString GetProtoMessagePkgStr(FMessageDefPtr MsgDef) const
		{
			FString MsgFullNameStr = MsgDef.FullName();
			MsgFullNameStr.ReplaceCharInline(TEXT('.'), TEXT('/'));
			return GetPackagePath(MsgFullNameStr);
		}
		FString GetProtoEnumPkgStr(FEnumDefPtr EnumDef) const
		{
			FString EnumFullNameStr = EnumDef.FullName();
			TArray<FString> EnumNameList;
			EnumFullNameStr.ParseIntoArray(EnumNameList, TEXT("."));
			return GetPackagePath(FString::Join(EnumNameList, TEXT("/")));
		}
		FString GetProtoDescPkgStr(FFileDefPtr FileDef, FString* OutName = nullptr) const
		{
			FString FileDirStr = FileDef.Package();
			TArray<FString> FileDirList;
			FileDirStr.ParseIntoArray(FileDirList, TEXT("."));
			FString FileNameStr = FileDef.Name();
			FileNameStr.ReplaceCharInline(TEXT('.'), TEXT('_'));
			int32 Idx = INDEX_NONE;
			if (FileNameStr.FindLastChar(TEXT('/'), Idx))
			{
				FileNameStr.MidInline(Idx + 1);
			}
			FileNameStr = FString::Printf(TEXT("PB_%s"), *FileNameStr);
			if (OutName)
				*OutName = FileNameStr;
			FString DescAssetPath = GetPackagePath(FString::Join(FileDirList, TEXT("/")) / FileNameStr);
			return DescAssetPath;
		}

		void TravelProtoEnum(TSet<FString>& Sets, FEnumDefPtr EnumDef) const
		{
			auto Str = GetProtoEnumPkgStr(EnumDef);
			if (Sets.Contains(Str))
				return;
			Sets.Add(Str);
		}
		void TravelProtoMessage(TSet<FString>& Sets, FMessageDefPtr MsgDef) const
		{
			auto Str = GetProtoMessagePkgStr(MsgDef);
			if (Sets.Contains(Str))
				return;
			Sets.Add(Str);

			for (auto FieldIndex = 0; FieldIndex < MsgDef.FieldCount(); ++FieldIndex)
			{
				auto FieldDef = MsgDef.FindFieldByNumber(FieldIndex);
				if (!FieldDef)
					continue;

				if (FieldDef.GetCType() == kUpb_CType_Enum)
				{
					auto SubEnumDef = FieldDef.EnumSubdef();
					TravelProtoEnum(Sets, SubEnumDef);
				}
				else if (FieldDef.GetCType() == kUpb_CType_Message)
				{
					auto SubMsgDef = FieldDef.MessageSubdef();
					TravelProtoMessage(Sets, SubMsgDef);
				}
			}  // for
		}

		void TravelProtoFile(TSet<FString>& Sets, FFileDefPtr FileDef) const
		{
			auto Str = GetProtoDescPkgStr(FileDef);
			if (Sets.Contains(Str))
				return;
			Sets.Add(Str);

			for (auto i = 0; i < FileDef.DependencyCount(); ++i)
			{
				auto DepFileDef = FileDef.Dependency(i);
				if (!DepFileDef)
					continue;
				TravelProtoFile(Sets, DepFileDef);
			}

			for (auto i = 0; i < FileDef.ToplevelEnumCount(); ++i)
			{
				auto EnumDef = FileDef.ToplevelEnum(i);
				if (!EnumDef)
					continue;
				TravelProtoEnum(Sets, EnumDef);
			}

			for (auto i = 0; i < FileDef.ToplevelMessageCount(); ++i)
			{
				auto MsgDef = FileDef.ToplevelMessage(i);
				if (!MsgDef)
					continue;
				TravelProtoMessage(Sets, MsgDef);
			}
		}

		FString RootPath = TEXT("/Game/ProtoStructs");
		TMap<const upb_FileDef*, upb_StringView> DescMap;

	public:
		bool DeleteAssetFile(const FString& PkgPath)
		{
			FString FilePath;
			if (!FPackageName::DoesPackageExist(PkgPath, &FilePath))
				return false;
			return IPlatformFile::GetPlatformPhysical().DeleteFile(*FilePath);
		}

		TArray<FString> GatherAssets(TArray<FFileDefPtr> FileDefs) const
		{
			TSet<FString> Sets;
			for (auto FileDef : FileDefs)
				TravelProtoFile(Sets, FileDef);
			return Sets.Array();
		}
		void SetAssetDir(const FString& AssetDir) { RootPath = AssetDir; }
		FProtoTraveler() {}
		FProtoTraveler(const TMap<const upb_FileDef*, upb_StringView>& InDescMap)
			: DescMap(InDescMap)
		{
		}
	};

	class FProtoGenerator : public FProtoTraveler
	{
		TMap<const upb_FileDef*, UProtoDescrotor*> FileDefMap;
		TMap<const upb_MessageDef*, UUserDefinedStruct*> MsgDefs;
		TMap<const upb_EnumDef*, UUserDefinedEnum*> EnumDefs;
		TSet<UUserDefinedStruct*> UserStructs;
		FEdGraphPinType FillBasicInfo(FFieldDefPtr FieldDef, UProtoDescrotor* Desc, FString& DefaultVal, bool bRefresh)
		{
			FEdGraphPinType PinType;
			PinType.ContainerType = FieldDef.IsRepeated() ? EPinContainerType::Array : EPinContainerType::None;
			switch (FieldDef.GetCType())
			{
				case kUpb_CType_Bool:
					PinType.PinCategory = UEdGraphSchema_K2::PC_Boolean;
					DefaultVal = LexToString(FieldDef.DefaultValue().bool_val);
					break;
				case kUpb_CType_Float:
#if UE_VERSION_NEWER_THAN(5, 0, 0)
					PinType.PinCategory = UEdGraphSchema_K2::PC_Real;
					PinType.PinSubCategory = UEdGraphSchema_K2::PC_Float;
#else
					PinType.PinCategory = UEdGraphSchema_K2::PC_Float;
#endif
					DefaultVal = LexToString(FieldDef.DefaultValue().float_val);
					break;
				case kUpb_CType_Double:
#if UE_VERSION_NEWER_THAN(5, 0, 0)
					PinType.PinCategory = UEdGraphSchema_K2::PC_Real;
					PinType.PinSubCategory = UEdGraphSchema_K2::PC_Double;
#else
					PinType.PinCategory = UEdGraphSchema_K2::PC_Double;
#endif
					DefaultVal = LexToString(FieldDef.DefaultValue().double_val);
					break;
				case kUpb_CType_Int32:
				case kUpb_CType_UInt32:
					PinType.PinCategory = UEdGraphSchema_K2::PC_Int;
					DefaultVal = LexToString(FieldDef.DefaultValue().int32_val);
					break;
				case kUpb_CType_Int64:
				case kUpb_CType_UInt64:
					PinType.PinCategory = UEdGraphSchema_K2::PC_Int64;
					DefaultVal = LexToString(FieldDef.DefaultValue().int64_val);
					break;
				case kUpb_CType_String:
					PinType.PinCategory = UEdGraphSchema_K2::PC_String;
					DefaultVal = StringView(FieldDef.DefaultValue().str_val);
					break;
				case kUpb_CType_Bytes:
				{
					ensure(!FieldDef.IsRepeated());
					PinType.PinCategory = UEdGraphSchema_K2::PC_Byte;
					PinType.ContainerType = EPinContainerType::Array;
				}
				break;
				case kUpb_CType_Enum:
				{
					PinType.PinCategory = UEdGraphSchema_K2::PC_Byte;
					auto SubEnumDef = FieldDef.EnumSubdef();
					auto ProtoDesc = AddProtoFileImpl(SubEnumDef.FileDef(), bRefresh);
					PinType.PinSubCategoryObject = AddProtoEnum(SubEnumDef, ProtoDesc, bRefresh);
					PinType.PinSubCategory = PinType.PinSubCategoryObject->GetFName();

					DefaultVal = SubEnumDef.Value(FieldDef.DefaultValue().int32_val).Name();
				}
				break;
				case kUpb_CType_Message:
				{
					if (FieldDef.IsMap())
					{
						auto MapEntryDef = FieldDef.MapEntrySubdef();
						FString IgnoreDfault;
						FFieldDefPtr KeyDef = MapEntryDef.MapKeyDef();
						ensure(!KeyDef.IsSubMessage());
						PinType = FillBasicInfo(KeyDef, Desc, IgnoreDfault, bRefresh);
						auto ValueType = FillBasicInfo(MapEntryDef.MapValueDef(), Desc, IgnoreDfault, bRefresh);
						PinType.PinValueType = FEdGraphTerminalType::FromPinType(ValueType);
						PinType.ContainerType = EPinContainerType::Map;
					}
					else
					{
						PinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
						auto SubMsgDef = FieldDef.MessageSubdef();
						auto ProtoDesc = AddProtoFileImpl(SubMsgDef.FileDef(), bRefresh);
						PinType.PinSubCategoryObject = AddProtoMessage(SubMsgDef, ProtoDesc, bRefresh);
						PinType.PinSubCategory = PinType.PinSubCategoryObject->GetFName();
					}
				}
				break;
			}

			return PinType;
		}

		UUserDefinedStruct* AddProtoMessage(FMessageDefPtr MsgDef, UProtoDescrotor* Desc, bool bRefresh)
		{
			check(MsgDef);

			for (auto i = 0; i < MsgDef.NestedEnumCount(); ++i)
			{
				auto NestedEnumDef = MsgDef.NestedEnum(i);
				if (!ensureAlways(NestedEnumDef))
					continue;
				AddProtoEnum(NestedEnumDef, Desc, bRefresh);
			}
			for (auto i = 0; i < MsgDef.NestedMessageCount(); ++i)
			{
				auto NestedMessageDef = MsgDef.NestedMessage(i);
				if (!ensureAlways(NestedMessageDef))
					continue;
				AddProtoMessage(NestedMessageDef, Desc, bRefresh);
			}

			static bool bRenameLater = true;
			static auto CreateProtoDefinedStruct = [](UPackage* InParent, FMessageDefPtr InMsgDef, UProtoDescrotor* InDesc, EObjectFlags Flags = RF_Public | RF_Standalone | RF_Transactional) {
				UProtoDefinedStruct* Struct = nullptr;
				if (ensure(FStructureEditorUtils::UserDefinedStructEnabled()))
				{
					Struct = NewObject<UProtoDefinedStruct>(InParent, InMsgDef.Name(), Flags);
					check(Struct);
					Struct->FullName = InMsgDef.FullName();
					Struct->ProtoDesc = InDesc;

					Struct->EditorData = NewObject<UUserDefinedStructEditorData>(Struct, NAME_None, RF_Transactional);
					check(Struct->EditorData);

					Struct->Guid = FGuid::NewGuid();
					Struct->SetMetaData(TEXT("BlueprintType"), TEXT("true"));
					Struct->Bind();
					Struct->StaticLink(true);
					Struct->Status = UDSS_Error;

					if (bRenameLater)
						FStructureEditorUtils::AddVariable(Struct, FEdGraphPinType(UEdGraphSchema_K2::PC_Boolean, NAME_None, nullptr, EPinContainerType::None, false, FEdGraphTerminalType()));
				}
				return Struct;
			};
			static auto GenerateNameVariable = [](UUserDefinedStruct* Struct, const FString& NameBase, const FGuid Guid) -> FName {
				FString Result;
				if (!NameBase.IsEmpty())
				{
					if (!FName::IsValidXName(NameBase, INVALID_OBJECTNAME_CHARACTERS))
					{
						Result = MakeObjectNameFromDisplayLabel(NameBase, NAME_None).GetPlainNameString();
					}
					else
					{
						Result = NameBase;
					}
				}

				if (Result.IsEmpty())
				{
					Result = TEXT("MemberVar");
				}

				const uint32 UniqueNameId = CastChecked<UUserDefinedStructEditorData>(Struct->EditorData)->GenerateUniqueNameIdForMemberVariable();
				const FString FriendlyName = FString::Printf(TEXT("%s_%u"), *Result, UniqueNameId);
				const FName NameResult = *FString::Printf(TEXT("%s_%s"), *FriendlyName, *Guid.ToString(EGuidFormats::Digits));
				check(NameResult.IsValidXName(INVALID_OBJECTNAME_CHARACTERS));
				return NameResult;
			};

			FString MsgAssetPath = GetProtoMessagePkgStr(MsgDef);
			if (MsgDefs.Contains(*MsgDef))
			{
				return MsgDefs.FindChecked(*MsgDef);
			}

			FScopeMark ScopeMark(ScopeStack, MsgDef.FullName().ToFString());

			bool bPackageCreated = FPackageName::DoesPackageExist(MsgAssetPath);
			UPackage* StructPkg = bPackageCreated ? LoadPackage(nullptr, *MsgAssetPath, LOAD_NoWarn) : CreatePackage(*MsgAssetPath);
			ensure(StructPkg);
			UObject* OldStruct = StructPkg->FindAssetInPackage();
			UUserDefinedStruct* MsgStruct = CreateProtoDefinedStruct(StructPkg, MsgDef, Desc);
			MsgDefs.Add({*MsgDef, MsgStruct});
			UserStructs.Add(MsgStruct);

			auto OldDescs = FStructureEditorUtils::GetVarDesc(MsgStruct);

			if (OldDescs.Num() > 0 && MsgStruct->GetStructureSize() > 0)
				FStructureEditorUtils::CompileStructure(MsgStruct);

			TMap<FString, FGuid> NameList;
			for (auto FieldIndex = 0; FieldIndex < MsgDef.FieldCount(); ++FieldIndex)
			{
				auto FieldDef = MsgDef.Field(FieldIndex);
				if (!ensureAlways(FieldDef))
					continue;

				FString DefaultVal;
				auto PinType = FillBasicInfo(FieldDef, Desc, DefaultVal, bRefresh);
				FGuid VarGuid;
				auto FieldName = FieldDef.Name().ToFString();
				Algo::FindByPredicate(OldDescs, [&](const FStructVariableDescription& Desc) {
					FString MemberName = Desc.VarName.ToString();
					GMP::Serializer::StripUserDefinedStructName(MemberName);
					if (MemberName == FieldName)
					{
						VarGuid = Desc.VarGuid;
						return true;
					}
					return false;
				});

				if (!VarGuid.IsValid())
				{
					if (bRenameLater)
					{
						ensureAlways(FStructureEditorUtils::AddVariable(MsgStruct, PinType));
						VarGuid = FStructureEditorUtils::GetVarDesc(MsgStruct).Last().VarGuid;
					}
					else
					{
						FStructureEditorUtils::ModifyStructData(MsgStruct);
						FStructVariableDescription NewVar;
						VarGuid = FGuid::NewGuid();
						NewVar.VarName = GenerateNameVariable(MsgStruct, FieldDef.Name(), VarGuid);
						NewVar.FriendlyName = FieldDef.Name();
						NewVar.SetPinType(PinType);
						NewVar.VarGuid = VarGuid;
						FStructureEditorUtils::GetVarDesc(MsgStruct).Add(NewVar);
					}
					if (!DefaultVal.IsEmpty())
						FStructureEditorUtils::ChangeVariableDefaultValue(MsgStruct, VarGuid, DefaultVal);
					NameList.Add(FieldDef.Name(), VarGuid);
				}
				else
				{
					FStructureEditorUtils::ChangeVariableType(MsgStruct, VarGuid, PinType);
					FStructureEditorUtils::ChangeVariableDefaultValue(MsgStruct, VarGuid, DefaultVal);
					NameList.Add(FieldDef.Name(), FGuid());
				}
			}  // for

			if (OldStruct && OldStruct != MsgStruct)
			{
				TArray<UObject*> Olds{OldStruct};
				ObjectTools::ConsolidateObjects(MsgStruct, Olds, false);
				OldStruct->ClearFlags(RF_Standalone);
				OldStruct->RemoveFromRoot();
				OldStruct->Rename(nullptr, nullptr, REN_DontCreateRedirectors);
				MsgStruct->Rename(MsgDef.Name().ToFStringData(), StructPkg);
			}
			else if (!OldStruct)
			{
				if (bRenameLater)
				{
					for (auto& Pair : NameList)
					{
						if (Pair.Value.IsValid())
							FStructureEditorUtils::RenameVariable(MsgStruct, Pair.Value, Pair.Key);
					}
				}
				else
				{
					FStructureEditorUtils::OnStructureChanged(MsgStruct, FStructureEditorUtils::EStructureEditorChangeInfo::AddedVariable);
				}

				auto& Descs = FStructureEditorUtils::GetVarDesc(MsgStruct);
				auto RemovedCnt = Descs.RemoveAll([&](auto& Desc) {
					FString MemberName = Desc.VarName.ToString();
					GMP::Serializer::StripUserDefinedStructName(MemberName);
					return !NameList.Contains(MemberName);
				});
				if (RemovedCnt > 0)
				{
					FStructureEditorUtils::OnStructureChanged(MsgStruct, FStructureEditorUtils::EStructureEditorChangeInfo::RemovedVariable);
				}
			}

			if (MsgStruct->Status != UDSS_UpToDate)
			{
				UE_LOG(LogGMP, Error, TEXT("%s"), *MsgStruct->ErrorMessage);
			}

			FString Filename;
			if (ensureAlways(FPackageName::TryConvertLongPackageNameToFilename(MsgAssetPath, Filename, FPackageName::GetAssetPackageExtension())))
			{
				StructPkg->Modify();
#if UE_VERSION_NEWER_THAN(5, 0, 0)
				FSavePackageArgs SaveArgs;
				SaveArgs.TopLevelFlags = EObjectFlags::RF_Public | EObjectFlags::RF_Standalone;
				SaveArgs.Error = GError;
				UPackage::SavePackage(StructPkg, MsgStruct, *Filename, SaveArgs);
#else
				UPackage::SavePackage(StructPkg, MsgStruct, EObjectFlags::RF_Public | EObjectFlags::RF_Standalone, *Filename, GError, nullptr, true, true, SAVE_NoError);
#endif
				IAssetRegistry::Get()->AssetCreated(MsgStruct);
			}
			return MsgStruct;
		}

		UUserDefinedEnum* AddProtoEnum(FEnumDefPtr EnumDef, UProtoDescrotor* Desc, bool bRefresh)
		{
			check(EnumDef);

			FString EnumAssetPath = GetProtoEnumPkgStr(EnumDef);
			if (EnumDefs.Contains(*EnumDef))
			{
				return EnumDefs.FindChecked(*EnumDef);
			}

			FScopeMark ScopeMark(ScopeStack, EnumDef.FullName().ToFString());

			//RemoveOldAsset(*MsgAssetPath);
			bool bPackageCreated = FPackageName::DoesPackageExist(EnumAssetPath);
			UPackage* EnumPkg = bPackageCreated ? LoadPackage(nullptr, *EnumAssetPath, LOAD_NoWarn) : CreatePackage(*EnumAssetPath);
			ensure(EnumPkg);
			UUserDefinedEnum* EnumObj = FindObject<UUserDefinedEnum>(EnumPkg, *EnumAssetPath);
			if (!EnumObj)
			{
				static auto CreateUserDefinedEnum = [](UObject* InParent, FEnumDefPtr InEnumDef, UProtoDescrotor* InDesc, EObjectFlags Flags = RF_Public | RF_Standalone | RF_Transactional) {
					// Cast<UUserDefinedEnum>(FEnumEditorUtils::CreateUserDefinedEnum(InParent, InEnumDef.Name(), Flags));
					UProtoDefinedEnum* Enum = NewObject<UProtoDefinedEnum>(InParent, InEnumDef.Name(), Flags);
					Enum->FullName = InEnumDef.FullName();
					Enum->ProtoDesc = InDesc;
					TArray<TPair<FName, int64>> EmptyNames;
					Enum->SetEnums(EmptyNames, UEnum::ECppForm::Namespaced);
					Enum->SetMetaData(TEXT("BlueprintType"), TEXT("true"));
					return Enum;
				};

				EnumObj = CreateUserDefinedEnum(EnumPkg, EnumDef, Desc);
			}
			else
			{
				//Clear
			}
			EnumDefs.Add({*EnumDef, EnumObj});

			TArray<TPair<FName, int64>> Names;
			for (int32 i = 0; i < EnumDef.ValueCount(); ++i)
			{
				FEnumValDefPtr EnumValDef = EnumDef.Value(i);
				if (!ensureAlways(EnumValDef))
					continue;
				const FString FullNameStr = EnumObj->GenerateFullEnumName(EnumValDef.Name().ToFStringData());
				Names.Add({*FullNameStr, EnumValDef.Number()});
			}

			EnumObj->SetEnums(Names, UEnum::ECppForm::Namespaced, EEnumFlags::Flags, true);

			for (int32 i = 0; i < EnumDef.ValueCount(); ++i)
			{
				FEnumValDefPtr EnumValDef = EnumDef.Value(i);
				if (!ensureAlways(EnumValDef))
					continue;
				FEnumEditorUtils::SetEnumeratorDisplayName(EnumObj, EnumValDef.Number(), FText::FromString(EnumValDef.Name()));
			}

			FString Filename;
			if (ensureAlways(FPackageName::TryConvertLongPackageNameToFilename(EnumAssetPath, Filename, FPackageName::GetAssetPackageExtension())))
			{
				EnumPkg->Modify();
#if UE_VERSION_NEWER_THAN(5, 0, 0)
				FSavePackageArgs SaveArgs;
				SaveArgs.TopLevelFlags = EObjectFlags::RF_Public | EObjectFlags::RF_Standalone;
				SaveArgs.Error = GError;
				UPackage::SavePackage(EnumPkg, EnumObj, *Filename, SaveArgs);
#else
				UPackage::SavePackage(EnumPkg, EnumObj, EObjectFlags::RF_Public | EObjectFlags::RF_Standalone, *Filename, GError, nullptr, true, true, SAVE_NoError);
#endif
				IAssetRegistry::Get()->AssetCreated(EnumObj);
			}
			return EnumObj;
		}

		TPair<UProtoDescrotor*, bool> AddProtoDesc(FFileDefPtr FileDef, bool bFroce)
		{
			FString FileNameStr;
			FString DescAssetPath = GetProtoDescPkgStr(FileDef, &FileNameStr);

			//RemoveOldAsset(*MsgAssetPath);
			bool bPackageCreated = FPackageName::DoesPackageExist(DescAssetPath);
			UPackage* DescPkg = bPackageCreated ? LoadPackage(nullptr, *DescAssetPath, LOAD_NoWarn) : CreatePackage(*DescAssetPath);
			ensure(DescPkg);
			UProtoDescrotor* OldDescObj = FindObject<UProtoDescrotor>(DescPkg, *FileNameStr);
			if (OldDescObj && !bFroce && StringView(DescMap.FindChecked(*FileDef)) == OldDescObj->Desc)
			{
				return {OldDescObj, true};
			}

			return {NewObject<UProtoDescrotor>(DescPkg, *FileNameStr, RF_Public | RF_Standalone), false};
		}

		void SaveProtoDesc(UProtoDescrotor* DescObj, FFileDefPtr FileDef, TArray<UProtoDescrotor*> Deps)
		{
			DescObj->Deps = Deps;

			upb_StringView DescView = DescMap.FindChecked(*FileDef);
			DescObj->Desc.AddUninitialized(DescView.size);
			FMemory::Memcpy(DescObj->Desc.GetData(), DescView.data, DescObj->Desc.Num());

			FString FileNameStr;
			FString DescAssetPath = GetProtoDescPkgStr(FileDef, &FileNameStr);
			auto Pkg = FindPackage(nullptr, *DescAssetPath);
			UProtoDescrotor* OldDescObj = FindObject<UProtoDescrotor>(Pkg, *FileNameStr);
			if (OldDescObj && OldDescObj != DescObj)
			{
				TArray<UObject*> Olds{OldDescObj};
				ObjectTools::ConsolidateObjects(DescObj, Olds, false);
				OldDescObj->ClearFlags(RF_Standalone);
				OldDescObj->RemoveFromRoot();
				OldDescObj->Rename(nullptr, nullptr, REN_DontCreateRedirectors);
				DescObj->Rename(*FileNameStr, Pkg);
			}

			FString Filename;
			if (ensureAlways(FPackageName::TryConvertLongPackageNameToFilename(Pkg->GetPathName(), Filename, FPackageName::GetAssetPackageExtension())))
			{
				Pkg->Modify();
#if UE_VERSION_NEWER_THAN(5, 0, 0)
				FSavePackageArgs SaveArgs;
				SaveArgs.TopLevelFlags = EObjectFlags::RF_Public | EObjectFlags::RF_Standalone;
				SaveArgs.Error = GError;
				UPackage::SavePackage(Pkg, DescObj, *Filename, SaveArgs);
#else
				UPackage::SavePackage(Pkg, DescObj, EObjectFlags::RF_Public | EObjectFlags::RF_Standalone, *Filename, GError, nullptr, true, true, SAVE_NoError);
#endif
				IAssetRegistry::Get()->AssetCreated(DescObj);
			}
		}

		UProtoDescrotor* AddProtoFileImpl(FFileDefPtr FileDef, bool bRefresh)
		{
			check(FileDef);
			if (FileDefMap.Contains(*FileDef))
				return FileDefMap.FindChecked(*FileDef);

			FScopeMark ScopeMark(ScopeStack, FileDef.Package().ToFString() / FileDef.Name().ToFString());

			auto DescPair = AddProtoDesc(FileDef, bRefresh);
			UProtoDescrotor* ProtoDesc = DescPair.Key;
			FileDefMap.Add({*FileDef, ProtoDesc});

			if (DescPair.Value)
			{
				TMap<FName, TArray<FName>> RefMap;
				TMap<FName, TArray<FName>> DepMap;
				auto PkgName = FName(*ProtoDesc->GetPackage()->GetPathName());
				FEditorUtils::GetReferenceAssets(nullptr, {PkgName.ToString()}, RefMap, DepMap, false);
				if (auto Find = RefMap.Find(PkgName))
				{
					for (auto& AssetName : *Find)
					{
						auto Struct = LoadObject<UProtoDefinedStruct>(nullptr, *AssetName.ToString());
						if (!Struct)
							continue;
						UserStructs.Add(Struct);
					}
				}
				return ProtoDesc;
			}

			TArray<UProtoDescrotor*> Deps;
			for (auto i = 0; i < FileDef.DependencyCount(); ++i)
			{
				auto DepFileDef = FileDef.Dependency(i);
				if (!ensureAlways(DepFileDef))
					continue;
				auto Desc = AddProtoFileImpl(DepFileDef, bRefresh);
				if (!ensureAlways(Desc))
					continue;
				Deps.Add(Desc);
			}
			SaveProtoDesc(ProtoDesc, FileDef, Deps);

			for (auto i = 0; i < FileDef.ToplevelEnumCount(); ++i)
			{
				auto EnumDef = FileDef.ToplevelEnum(i);
				if (!ensureAlways(EnumDef))
					continue;
				AddProtoEnum(EnumDef, ProtoDesc, bRefresh);
			}

			for (auto i = 0; i < FileDef.ToplevelMessageCount(); ++i)
			{
				auto MsgDef = FileDef.ToplevelMessage(i);
				if (!ensureAlways(MsgDef))
					continue;
				AddProtoMessage(MsgDef, ProtoDesc, bRefresh);
			}
			return ProtoDesc;
		}

	public:
		FProtoGenerator(const TMap<const upb_FileDef*, upb_StringView>& In)
			: FProtoTraveler(In)
		{
		}

		void AddProtoFile(FFileDefPtr FileDef, bool bRefresh = false) { AddProtoFileImpl(FileDef, bRefresh); }
		TSet<UUserDefinedStruct*> GetUserDefinedStructs() const { return UserStructs; }

		TArray<FString> ScopeStack;
		struct FScopeMark
		{
			TArray<FString>& StackRef;
			FString Str;
			int32 Lv;
			FScopeMark(TArray<FString>& Stack, FString InStr)
				: StackRef(Stack)
				, Str(MoveTemp(InStr))
			{
				StackRef.Add(Str);
				Lv = StackRef.Num();
				UE_LOG(LogGMP, Display, TEXT("ScopeMark : %s"), *Str);
			}
			~FScopeMark()
			{
				if (ensureAlways(Lv == StackRef.Num() && StackRef.Last() == Str))
					StackRef.Pop();
			}
		};
	};

	static void GeneratePBStruct(UWorld* InWorld)
	{
		auto& Pair = ResetDefPool();

		TMap<const upb_FileDef*, upb_StringView> Storages;
		TArray<FFileDefPtr> FileDefs = upb::generator::FillDefPool(Pair->DefPool, Storages);

		auto AssetToUnload = FProtoTraveler(Storages).GatherAssets(FileDefs);
		if (!ensure(AssetToUnload.Num()))
			return;

		FEditorUtils::DeletePackages(InWorld, AssetToUnload, CreateWeakLambda(InWorld, [Storages, FileDefs](bool bSucc, TArray<FString> AllUnloadedList) {
										 if (!bSucc)
											 return;
										 TArray<FString> FilePaths;
										 for (auto& ResId : AllUnloadedList)
										 {
											 FString FilePath;
											 if (FPackageName::DoesPackageExist(ResId, &FilePath))
											 {
												 FilePaths.Add(MoveTemp(FilePath));
											 }
										 }
										 if (FilePaths.Num())
											 IAssetRegistry::Get()->ScanFilesSynchronous(FilePaths, true);

										 FScopedTransaction ScopedTransaction(NSLOCTEXT("GMPProto", "GeneratePBStruct", "GeneratePBStruct"));
										 bool bRefresh = false;
										 FProtoGenerator ProtoGenerator(Storages);
										 for (auto FileDef : FileDefs)
											 ProtoGenerator.AddProtoFile(FileDef, bRefresh);

										 IAssetRegistry::Get()->ScanFilesSynchronous(FilePaths, true);

										 for (auto UserStruct : ProtoGenerator.GetUserDefinedStructs())
										 {
											 FStructOnScope StructOnScopeFrom;
											 StructOnScopeFrom.Initialize(UserStruct);
											 TArray<uint8> Buffer;
											 Serializer::UStructToProtoImpl(Buffer, UserStruct, StructOnScopeFrom.GetStructMemory());

											 FStructOnScope StructOnScopeTo;
											 StructOnScopeTo.Initialize(UserStruct);
											 Deserializer::UStructFromProtoImpl(Buffer, UserStruct, StructOnScopeTo.GetStructMemory());

											 ensureAlways(UserStruct->CompareScriptStruct(StructOnScopeFrom.GetStructMemory(), StructOnScopeTo.GetStructMemory(), CPF_None));
										 }
									 }));
	}

	FAutoConsoleCommandWithWorld XVar_GeneratePBStruct(TEXT("x.gmp.proto.gen"), TEXT(""), FConsoleCommandWithWorldDelegate::CreateStatic(GeneratePBStruct));
}  // namespace PB
}  // namespace GMP
#endif  // GMP_EXTEND_CONSOLE

#if defined(PROTOBUF_API)
#pragma warning(push)
#pragma warning(disable : 4800)
#pragma warning(disable : 4125)
#pragma warning(disable : 4647)
#pragma warning(disable : 4668)
#pragma warning(disable : 4582)
#pragma warning(disable : 4583)
#pragma warning(disable : 4946)
#pragma warning(disable : 4577)
#pragma warning(disable : 4996)
#ifndef __GLIBCXX__
#define __GLIBCXX__ 0
#endif
#include <google/protobuf/compiler/importer.h>
#include <google/protobuf/descriptor.pb.h>
#pragma warning(pop)

namespace GMP
{
namespace PB
{
	static TArray<TArray<uint8>> GatherFileDescriptorProtosForDir(FString RootDir)
	{
		TArray<TArray<uint8>> ProtoDescriptors;
		TArray<FString> ProtoFiles;
		IPlatformFile::GetPlatformPhysical().FindFilesRecursively(ProtoFiles, *RootDir, TEXT(".proto"));
		if (ProtoFiles.Num() > 0)
		{
			using namespace google::protobuf;
			struct FErrorCollector final : public compiler::MultiFileErrorCollector
			{
			public:
				virtual void AddWarning(const std::string& filename, int line, int column, const std::string& message) override {}
				virtual void AddError(const std::string& filename, int line, int column, const std::string& message) override
				{
					UE_LOG(LogGMP, Error, TEXT("%s(%d:%d) : %s"), UTF8_TO_TCHAR(filename.c_str()), line, column, UTF8_TO_TCHAR(message.c_str()));
				}
			};
			FErrorCollector Error;
			compiler::DiskSourceTree SrcTree;
			SrcTree.MapPath("", TCHAR_TO_UTF8(*FPaths::ConvertRelativePathToFull(RootDir)));

			compiler::SourceTreeDescriptorDatabase Database(&SrcTree);
			Database.RecordErrorsTo(&Error);

			if (!RootDir.EndsWith(TEXT("/")))
				RootDir.AppendChar('/');
			for (auto& ProtoFile : ProtoFiles)
			{
				FPaths::MakePathRelativeTo(ProtoFile, *RootDir);
				FileDescriptorProto DescProto;
				if (!ensure(Database.FindFileByName(TCHAR_TO_UTF8(*ProtoFile), &DescProto)))
					continue;

				auto Size = DescProto.ByteSizeLong();
				if (!ensure(Size > 0))
					continue;

				TArray<uint8> ProtoDescriptor;
				ProtoDescriptor.AddUninitialized(Size);
				if (!ensure(DescProto.SerializeToArray((char*)ProtoDescriptor.GetData(), Size)))
					continue;
				ProtoDescriptors.Add(MoveTemp(ProtoDescriptor));
			}
		}
		return ProtoDescriptors;
	}
	FAutoConsoleCommandWithWorld XVar_GatherProtos(TEXT("x.gmp.proto.gathergen"),
												   TEXT(""),  //
												   FConsoleCommandWithWorldDelegate::CreateLambda([](UWorld* InWorld) {
													   void* ParentWindowHandle = FSlateApplication::Get().GetActiveTopLevelWindow()->GetNativeWindow()->GetOSWindowHandle();
													   IDesktopPlatform* DesktopPlatform = FDesktopPlatformModule::Get();

													   FString OutFolderPath;
													   static TOptional<FString> DefaultPath;
													   if (!DesktopPlatform->OpenDirectoryDialog(ParentWindowHandle, TEXT("please choose proto's root directory"), DefaultPath.Get(FPaths::ProjectContentDir()), OutFolderPath))
														   return;
													   DefaultPath = OutFolderPath;

													   auto Descs = GatherFileDescriptorProtosForDir(OutFolderPath);
													   if (!Descs.Num())
														   return;

													   auto& PreGenerator = upb::generator::GetPreGenerator();
													   PreGenerator.Reset();
													   for (auto& Desc : Descs)
													   {
														   PreGenerator.PreAddProtoDesc(Desc);
													   }
													   GeneratePBStruct(InWorld);
												   }));
}  // namespace PB
}  // namespace GMP
#endif  // defined(PROTOBUF_API)
#endif  // WITH_EDITOR
