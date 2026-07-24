#include "TypeContext.h"

TypeContext::TypeContext() {
    errorType = allocate(TypeKind::Error);
    nullType = allocate(TypeKind::Null);
}

Type* TypeContext::allocate(TypeKind k, Type* inner) {
    auto t = std::make_unique<Type>(k, inner);
    Type* raw = t.get();
    ownedTypes.push_back(std::move(t));
    return raw;
}

Type* TypeContext::getPrimitive(TypeKind k) {
    auto it = primitiveCache.find(static_cast<int>(k));
    if (it != primitiveCache.end()) return it->second;
    Type* t = allocate(k);
    primitiveCache[static_cast<int>(k)] = t;
    return t;
}

Type* TypeContext::getOptional(Type* inner) {
    if (!inner || inner->isError()) return errorType;
    if (inner->isOptional()) return inner;  // T?? collapses to T?
    auto it = optionalCache.find(inner);
    if (it != optionalCache.end()) return it->second;
    Type* t = allocate(TypeKind::Optional, inner);
    optionalCache[inner] = t;
    return t;
}

Type* TypeContext::getArray(Type* element) {
    if (!element || element->isError()) return errorType;
    if (element->isVoid()) return errorType;
    auto it = arrayCache.find(element);
    if (it != arrayCache.end()) return it->second;
    Type* t = allocate(TypeKind::Array, element);
    arrayCache[element] = t;
    return t;
}

Type* TypeContext::primitiveFromName(const std::u16string& name) {
    if (name == u"bool")    return getPrimitive(TypeKind::Bool);
    if (name == u"byte")    return getPrimitive(TypeKind::Byte);
    if (name == u"short")   return getPrimitive(TypeKind::Short);
    if (name == u"ushort")  return getPrimitive(TypeKind::UShort);
    if (name == u"int")     return getPrimitive(TypeKind::Int);
    if (name == u"uint")    return getPrimitive(TypeKind::UInt);
    if (name == u"long")    return getPrimitive(TypeKind::Long);
    if (name == u"ulong")   return getPrimitive(TypeKind::ULong);
    if (name == u"float")   return getPrimitive(TypeKind::Float);
    if (name == u"double")  return getPrimitive(TypeKind::Double);
    if (name == u"decimal") return getPrimitive(TypeKind::Decimal);
    if (name == u"char")    return getPrimitive(TypeKind::Char);
    if (name == u"string")  return getPrimitive(TypeKind::String);
    if (name == u"void")    return getPrimitive(TypeKind::Void);
    return nullptr;
}

Type* TypeContext::registerStruct(const std::u16string& modulePath, std::u16string name) {
    auto info = std::make_unique<StructInfo>();
    info->name = name;
    info->modulePath = modulePath;
    StructInfo* infoPtr = info.get();
    ownedStructs.push_back(std::move(info));

    Type* t = allocate(TypeKind::Struct);
    t->structInfo = infoPtr;
    structCache[Key{modulePath, std::move(name)}] = t;
    return t;
}

Type* TypeContext::lookupStruct(const std::u16string& modulePath, const std::u16string& name) const {
    auto it = structCache.find(Key{modulePath, name});
    return it == structCache.end() ? nullptr : it->second;
}

Type* TypeContext::registerClass(const std::u16string& modulePath, std::u16string name) {
    auto info = std::make_unique<StructInfo>();
    info->name = name;
    info->modulePath = modulePath;
    StructInfo* infoPtr = info.get();
    ownedStructs.push_back(std::move(info));

    Type* t = allocate(TypeKind::Class);
    t->structInfo = infoPtr;
    classCache[Key{modulePath, std::move(name)}] = t;
    return t;
}

Type* TypeContext::lookupClass(const std::u16string& modulePath, const std::u16string& name) const {
    auto it = classCache.find(Key{modulePath, name});
    return it == classCache.end() ? nullptr : it->second;
}

Type* TypeContext::registerInterface(const std::u16string& modulePath, std::u16string name) {
    Type* t = registerClass(modulePath, std::move(name));
    t->structInfo->isInterface = true;
    t->structInfo->isAbstract = true;  // never instantiable
    return t;
}

Type* TypeContext::registerExternalType(const std::u16string& modulePath, std::u16string name) {
    auto info = std::make_unique<StructInfo>();
    info->name = name;
    info->modulePath = modulePath;
    StructInfo* infoPtr = info.get();
    ownedStructs.push_back(std::move(info));

    Type* t = allocate(TypeKind::External);
    t->structInfo = infoPtr;
    externalCache[Key{modulePath, std::move(name)}] = t;
    return t;
}

Type* TypeContext::lookupExternalType(const std::u16string& modulePath, const std::u16string& name) const {
    auto it = externalCache.find(Key{modulePath, name});
    return it == externalCache.end() ? nullptr : it->second;
}

Type* TypeContext::registerEnum(const std::u16string& modulePath, std::u16string name) {
    auto info = std::make_unique<StructInfo>();
    info->name = name;
    info->modulePath = modulePath;
    StructInfo* infoPtr = info.get();
    ownedStructs.push_back(std::move(info));

    Type* t = allocate(TypeKind::Enum);
    t->structInfo = infoPtr;
    enumCache[Key{modulePath, std::move(name)}] = t;
    return t;
}

Type* TypeContext::lookupEnum(const std::u16string& modulePath, const std::u16string& name) const {
    auto it = enumCache.find(Key{modulePath, name});
    return it == enumCache.end() ? nullptr : it->second;
}

Type* TypeContext::lookupNamedType(const std::u16string& modulePath, const std::u16string& name) const {
    if (Type* t = lookupStruct(modulePath, name)) return t;
    if (Type* t = lookupClass(modulePath, name)) return t;
    if (Type* t = lookupEnum(modulePath, name)) return t;
    return lookupExternalType(modulePath, name);
}

Type* TypeContext::getTypeParam(const void* owner, int index, std::u16string name,
                                const std::vector<StructInfo*>& bounds) {
    // The primary bound drives single-bound lookups: the class bound when one
    // exists, otherwise the first interface bound.
    StructInfo* primary = nullptr;
    for (StructInfo* b : bounds) {
        if (b && !b->isInterface) { primary = b; break; }
    }
    if (!primary && !bounds.empty()) primary = bounds.front();

    auto key = std::make_pair(owner, index);
    auto it = typeParamCache.find(key);
    if (it != typeParamCache.end()) {
        if (primary && !it->second->structInfo) it->second->structInfo = primary;
        if (!bounds.empty() && it->second->paramBounds.empty()) it->second->paramBounds = bounds;
        return it->second;
    }
    Type* t = allocate(TypeKind::TypeParam);
    t->paramOwner = owner;
    t->paramIndex = index;
    t->paramName = std::move(name);
    t->structInfo = primary;
    t->paramBounds = bounds;
    typeParamCache[key] = t;
    return t;
}

Type* TypeContext::substitute(Type* t, const void* owner, const std::vector<Type*>& args) {
    if (!t) return t;
    switch (t->kind) {
        case TypeKind::TypeParam:
            if (t->paramOwner == owner && t->paramIndex >= 0 &&
                t->paramIndex < static_cast<int>(args.size())) {
                return args[t->paramIndex];
            }
            return t;
        case TypeKind::Array:
            return getArray(substitute(t->inner, owner, args));
        case TypeKind::Optional:
            return getOptional(substitute(t->inner, owner, args));
        case TypeKind::Struct:
        case TypeKind::Class:
            if (t->structInfo && t->structInfo->templateOf) {
                std::vector<Type*> newArgs;
                newArgs.reserve(t->structInfo->typeArgs.size());
                bool changed = false;
                for (Type* a : t->structInfo->typeArgs) {
                    Type* s = substitute(a, owner, args);
                    changed |= (s != a);
                    newArgs.push_back(s);
                }
                if (!changed) return t;
                return instantiateInternal(t->structInfo->templateOf, t->kind, newArgs);
            }
            return t;
        default:
            return t;
    }
}

Type* TypeContext::instantiate(Type* templateType, const std::vector<Type*>& args) {
    if (!templateType || !templateType->structInfo) return errorType;
    return instantiateInternal(templateType->structInfo, templateType->kind, args);
}

Type* TypeContext::instantiateInternal(StructInfo* templ, TypeKind kind,
                                       const std::vector<Type*>& args) {
    InstantiationKey key{templ, args};
    auto it = instantiationCache.find(key);
    if (it != instantiationCache.end()) return it->second;

    if (static_cast<int>(instantiationChain_.size()) >= kMaxInstantiationDepth) {
        recordInstantiationOverflow();
        return errorType;
    }

    auto info = std::make_unique<StructInfo>();
    info->name = templ->name;
    info->modulePath = templ->modulePath;
    info->packagePrefix = templ->packagePrefix;
    info->visibility = templ->visibility;
    info->isAbstract = templ->isAbstract;
    info->isFinal = templ->isFinal;
    info->isSealed = templ->isSealed;
    info->isInterface = templ->isInterface;
    info->templateOf = templ;
    info->typeArgs = args;
    info->baseInfo = templ->baseInfo;
    info->baseFieldCount = templ->baseFieldCount;
    StructInfo* inst = info.get();
    ownedStructs.push_back(std::move(info));

    // Register the shell before filling, so a self-referential field
    // (e.g. `Node<T>? next`) resolves back to this same instantiation.
    Type* t = allocate(kind);
    t->structInfo = inst;
    instantiationCache[key] = t;
    instantiationList_.push_back(t);
    instanceTypes_[inst] = t;

    // Fill now if the template's members are already resolved; otherwise defer
    // (the template is laid out later, then materializeInstantiations fills it).
    if (templ->membersCollected) {
        fillInstantiation(inst, templ, args);
    } else {
        pendingInstantiations_.push_back({inst, templ, args});
    }
    return t;
}

void TypeContext::fillInstantiation(StructInfo* inst, StructInfo* templ,
                                    const std::vector<Type*>& args) {
    auto selfIt = instanceTypes_.find(inst);
    instantiationChain_.push_back(selfIt == instanceTypes_.end() ? nullptr : selfIt->second);
    const void* owner = static_cast<const void*>(templ);
    // Base layout is settled by fill time; refresh what the shell copied at
    // creation, and rebind a generic base to its concrete instantiation.
    inst->baseInfo = templ->baseInfo;
    inst->baseFieldCount = templ->baseFieldCount;
    inst->vtableSize = templ->vtableSize;
    if (templ->baseInfo && templ->baseInfo->templateOf) {
        std::vector<Type*> baseArgs;
        baseArgs.reserve(templ->baseInfo->typeArgs.size());
        for (Type* a : templ->baseInfo->typeArgs) {
            baseArgs.push_back(substitute(a, owner, args));
        }
        Type* concreteBase = instantiateInternal(templ->baseInfo->templateOf,
                                                 TypeKind::Class, baseArgs);
        if (concreteBase && concreteBase->structInfo) {
            inst->baseInfo = concreteBase->structInfo;
        }
    }
    inst->implementedInterfaces.clear();
    for (Type* ifaceT : templ->implementedInterfaces) {
        inst->implementedInterfaces.push_back(substitute(ifaceT, owner, args));
    }
    for (const auto& f : templ->fields) {
        FieldInfo nf = f;
        nf.type = substitute(f.type, owner, args);
        nf.definingClass = inst;
        inst->fields.push_back(nf);
    }
    for (const auto& m : templ->methods) {
        MethodInfo nm = m;
        nm.definingClass = inst;
        if (m.symbol) {
            auto sym = std::make_unique<Symbol>(*m.symbol);
            sym->methodOwner = inst;
            for (auto& pt : sym->paramTypes) pt = substitute(pt, owner, args);
            sym->returnType = substitute(sym->returnType, owner, args);
            nm.symbol = sym.get();
            ownedSymbols.push_back(std::move(sym));
        }
        inst->methods.push_back(nm);
    }
    instantiationChain_.pop_back();
}

static bool fieldReferencesTemplate(const Type* t, const StructInfo* templ) {
    if (!t) return false;
    switch (t->kind) {
        case TypeKind::Optional:
        case TypeKind::Array:
            return fieldReferencesTemplate(t->inner, templ);
        case TypeKind::Struct:
        case TypeKind::Class:
            if (t->structInfo) {
                if (t->structInfo->templateOf == templ) return true;
                for (const Type* a : t->structInfo->typeArgs) {
                    if (fieldReferencesTemplate(a, templ)) return true;
                }
            }
            return false;
        default:
            return false;
    }
}

void TypeContext::recordInstantiationOverflow() {
    Type* root = instantiationChain_[0];
    Type* first = instantiationChain_[1];
    Type* second = instantiationChain_[2];
    StructInfo* templ = root && root->structInfo ? root->structInfo->templateOf : nullptr;
    for (const auto& o : instantiationOverflows_) {
        if (o.templ == templ) return;
    }

    std::string rootName = root ? root->toString() : std::string("?");
    std::string message = "Instantiating '" + rootName +
        "' never finishes: each instantiation requires another ('" + rootName + "' needs '" +
        (first ? first->toString() : std::string("?")) + "', which needs '" +
        (second ? second->toString() : std::string("?")) +
        "', ...). Break the recursive type argument.";

    int line = templ ? templ->line : 0;
    int column = templ ? templ->column : 0;
    int length = 1;
    if (templ) {
        for (const auto& f : templ->fields) {
            if (fieldReferencesTemplate(f.type, templ)) {
                line = f.line;
                column = f.column;
                length = static_cast<int>(f.name.size());
                break;
            }
        }
    }
    instantiationOverflows_.push_back({templ, std::move(message), line, column, length});
}

void TypeContext::recordFunctionInstantiation(Symbol* fn, std::vector<Type*> args) {
    for (auto& fi : functionInstantiations_) {
        if (fi.function == fn && fi.args == args) return;
    }
    functionInstantiations_.push_back({fn, std::move(args)});
}

bool TypeContext::materializeInstantiations() {
    bool did = false;
    while (!pendingInstantiations_.empty()) {
        PendingInstantiation p = pendingInstantiations_.back();
        pendingInstantiations_.pop_back();
        fillInstantiation(p.inst, p.templ, p.args);  // may enqueue nested instances
        did = true;
    }
    return did;
}

void TypeContext::refreshInstantiationInheritance() {
    for (Type* t : instantiationList_) {
        StructInfo* inst = t ? t->structInfo : nullptr;
        if (!inst || !inst->templateOf) continue;
        StructInfo* templ = inst->templateOf;
        inst->vtableSize = templ->vtableSize;
        size_t n = std::min(inst->methods.size(), templ->methods.size());
        for (size_t i = 0; i < n; ++i) {
            inst->methods[i].vtableSlot = templ->methods[i].vtableSlot;
            if (inst->methods[i].symbol && templ->methods[i].symbol) {
                inst->methods[i].symbol->abiThrows = templ->methods[i].symbol->abiThrows;
            }
        }
    }
}

void TypeContext::ensureFilled(StructInfo* inst) {
    for (size_t i = 0; i < pendingInstantiations_.size(); ++i) {
        if (pendingInstantiations_[i].inst != inst) continue;
        if (!pendingInstantiations_[i].templ->membersCollected) return;
        PendingInstantiation p = std::move(pendingInstantiations_[i]);
        pendingInstantiations_.erase(pendingInstantiations_.begin() +
                                     static_cast<ptrdiff_t>(i));
        fillInstantiation(p.inst, p.templ, p.args);
        return;
    }
}

bool TypeContext::containsTypeParam(const Type* t) {
    if (!t) return false;
    switch (t->kind) {
        case TypeKind::TypeParam:
            return true;
        case TypeKind::Array:
        case TypeKind::Optional:
            return containsTypeParam(t->inner);
        case TypeKind::Struct:
        case TypeKind::Class:
            if (t->structInfo && t->structInfo->templateOf) {
                for (const Type* a : t->structInfo->typeArgs) {
                    if (containsTypeParam(a)) return true;
                }
            }
            return false;
        default:
            return false;
    }
}
