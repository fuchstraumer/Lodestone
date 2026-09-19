// Standalone Slang-only probe. Not a unit test. It answers the load-bearing question for the
// FindDeclaringModule walk: when module A declares an enum and module B imports A and declares an
// extern-const of that enum type, is the enum decl's getType() in A the same interned TypeReflection
// pointer as the variable's VariableReflection::getType() in B? If yes, matching a declaration by
// pointer identity works across a module boundary, which is the whole basis of the fix.
//
// Links only against slang, so it builds while the cooker is mid-refactor. Build the target directly:
//   cmake --build build/<preset> --config <cfg> --target DeclKindProbe
#include <slang.h>
#include <slang-com-ptr.h>
#include <cstdio>
#include <cstring>

using Slang::ComPtr;

static const char* KindToString(slang::DeclReflection::Kind kind)
{
    switch (kind)
    {
    case slang::DeclReflection::Kind::Unsupported: return "Unsupported";
    case slang::DeclReflection::Kind::Struct:      return "Struct";
    case slang::DeclReflection::Kind::Func:        return "Func";
    case slang::DeclReflection::Kind::Module:      return "Module";
    case slang::DeclReflection::Kind::Generic:     return "Generic";
    case slang::DeclReflection::Kind::Variable:    return "Variable";
    case slang::DeclReflection::Kind::Namespace:   return "Namespace";
    case slang::DeclReflection::Kind::Enum:        return "Enum";
    default:                                       return "??";
    }
}

// Mirror of the real BelongsToModule/FindDeclaringModule: does the tree rooted at `decl` declare a
// decl whose getType() is pointer-equal to `type`?
static bool BelongsToModule(slang::TypeReflection* type, slang::DeclReflection* decl)
{
    const unsigned count = decl->getChildrenCount();
    for (unsigned i = 0; i < count; ++i)
    {
        slang::DeclReflection* child = decl->getChild(i);
        if (child->getType() == type)
        {
            return true;
        }
        if (child->getChildrenCount() > 0 && BelongsToModule(type, child))
        {
            return true;
        }
    }
    return false;
}

static const char* FindDeclaringModule(slang::ISession* session, slang::TypeReflection* type)
{
    for (SlangInt i = 0; i < session->getLoadedModuleCount(); ++i)
    {
        slang::IModule* module = session->getLoadedModule(i);
        if (module == nullptr)
        {
            continue;
        }
        if (BelongsToModule(type, module->getModuleReflection()))
        {
            return module->getName();
        }
    }
    return "";
}

int main()
{
    ComPtr<slang::IGlobalSession> globalSession;
    if (SLANG_FAILED(slang::createGlobalSession(globalSession.writeRef())))
    {
        printf("createGlobalSession failed\n");
        return 1;
    }

    slang::TargetDesc target{};
    target.format = SLANG_SPIRV;
    target.profile = globalSession->findProfile("spirv_1_4");

    slang::SessionDesc sessionDesc{};
    sessionDesc.targets = &target;
    sessionDesc.targetCount = 1;

    ComPtr<slang::ISession> session;
    if (SLANG_FAILED(globalSession->createSession(sessionDesc, session.writeRef())))
    {
        printf("createSession failed\n");
        return 1;
    }

    // Module A declares the enum. Module B imports A and declares an axis variable of the enum type.
    const char* typesSource = "public enum QualityTier { Low = 3, Medium = 1, High = 7 };\n";
    const char* userSource =
        "import ProbeTypes;\n"
        "extern static const QualityTier QUALITY = QualityTier::Low;\n"
        "RWStructuredBuffer<float> b;\n"
        "[shader(\"compute\")][numthreads(1,1,1)]\n"
        "void cs(uint3 t : SV_DispatchThreadID) { b[0] = float(int(QUALITY)); }\n";

    ComPtr<slang::IBlob> diag1;
    slang::IModule* typesModule =
        session->loadModuleFromSourceString("ProbeTypes", "ProbeTypes.slang", typesSource, diag1.writeRef());
    if (diag1 && diag1->getBufferSize() > 0)
    {
        printf("ProbeTypes diagnostics:\n%s\n", static_cast<const char*>(diag1->getBufferPointer()));
    }
    if (typesModule == nullptr)
    {
        printf("ProbeTypes load failed\n");
        return 1;
    }

    ComPtr<slang::IBlob> diag2;
    slang::IModule* userModule =
        session->loadModuleFromSourceString("ProbeUser", "ProbeUser.slang", userSource, diag2.writeRef());
    if (diag2 && diag2->getBufferSize() > 0)
    {
        printf("ProbeUser diagnostics:\n%s\n", static_cast<const char*>(diag2->getBufferPointer()));
    }
    if (userModule == nullptr)
    {
        printf("ProbeUser load failed\n");
        return 1;
    }

    // The axis code reaches the type as VariableReflection::getType() on the axis variable in module B.
    slang::DeclReflection* userDecl = userModule->getModuleReflection();
    slang::TypeReflection* axisType = nullptr;
    const unsigned count = userDecl->getChildrenCount();
    for (unsigned i = 0; i < count; ++i)
    {
        slang::DeclReflection* child = userDecl->getChild(i);
        const char* name = child->getName();
        if (name != nullptr && strcmp(name, "QUALITY") == 0 &&
            child->getKind() == slang::DeclReflection::Kind::Variable)
        {
            slang::VariableReflection* var = child->asVariable();
            axisType = var ? var->getType() : nullptr;
        }
    }

    printf("== cross-module identity ==\n");
    printf("axis variable QUALITY (in ProbeUser) value type = %p  name=%s  kind=%s\n",
           (void*)axisType,
           axisType && axisType->getName() ? axisType->getName() : "(none)",
           axisType ? "type" : "null");

    const char* declaring = FindDeclaringModule(session.get(), axisType);
    printf("FindDeclaringModule(...) = \"%s\"\n", declaring);
    printf("expected \"ProbeTypes\": %s\n", (strcmp(declaring, "ProbeTypes") == 0) ? "PASS" : "FAIL");

    // --- scalar type of the cross-module enum: reproduce the None, and test alternatives ---
    const int noneScalar = static_cast<int>(slang::TypeReflection::ScalarType::None);
    printf("\n== scalar type via the importing reference (what the cook does now) ==\n");
    printf("axisType->getKind()       = %d (Enum=%d)\n",
           static_cast<int>(axisType->getKind()), static_cast<int>(slang::TypeReflection::Kind::Enum));
    printf("axisType->getScalarType() = %d (None=%d)\n",
           static_cast<int>(axisType->getScalarType()), noneScalar);

    printf("\n== alternative: the enum's case field ==\n");
    if (axisType->getFieldCount() > 0)
    {
        slang::VariableReflection* c0 = axisType->getFieldByIndex(0);
        slang::TypeReflection* ct = c0 ? c0->getType() : nullptr;
        printf("case[0] name=%s  fieldType kind=%d scalar=%d\n",
               (c0 && c0->getName()) ? c0->getName() : "?",
               ct ? static_cast<int>(ct->getKind()) : -1,
               ct ? static_cast<int>(ct->getScalarType()) : -1);
        Slang::ComPtr<slang::IBlob> blob;
        if (c0 && SLANG_SUCCEEDED(c0->getDefaultValueBlob(blob.writeRef())) && blob)
        {
            printf("case[0] defaultValueBlob size = %zu bytes\n", blob->getBufferSize());
        }
    }

    printf("\n== alternative: the enum decl's own getType() in its declaring module ==\n");
    for (SlangInt i = 0; i < session->getLoadedModuleCount(); ++i)
    {
        slang::IModule* m = session->getLoadedModule(i);
        if (m == nullptr || m->getModuleReflection() == nullptr)
        {
            continue;
        }
        slang::DeclReflection* mr = m->getModuleReflection();
        for (unsigned j = 0; j < mr->getChildrenCount(); ++j)
        {
            slang::DeclReflection* ch = mr->getChild(j);
            if (ch->getKind() == slang::DeclReflection::Kind::Enum &&
                ch->getName() != nullptr && strcmp(ch->getName(), "QualityTier") == 0)
            {
                slang::TypeReflection* dt = ch->getType();
                printf("module %-12s enum decl getType()=%p scalar=%d  samePtrAsAxisType=%s\n",
                       m->getName(),
                       (void*)dt,
                       dt ? static_cast<int>(dt->getScalarType()) : -1,
                       (dt == axisType) ? "YES" : "NO");
            }
        }
    }

    return 0;
}
