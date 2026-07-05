#include "SyntaxKind.h"

#include <array>
#include <utility>

bool isToken(SyntaxKind k) {
    return k < SyntaxKind::FIRST_NODE_KIND;
}

bool isNode(SyntaxKind k) {
    return k > SyntaxKind::FIRST_NODE_KIND && k < SyntaxKind::LAST_KIND;
}

bool isTrivia(SyntaxKind k) {
    switch (k) {
        case SyntaxKind::Whitespace:
        case SyntaxKind::Newline:
        case SyntaxKind::LineComment:
        case SyntaxKind::BlockComment:
            return true;
        default:
            return false;
    }
}

bool isKeyword(SyntaxKind k) {
    return k >= SyntaxKind::KwBool && k <= SyntaxKind::KwWhile;
}

bool isLiteral(SyntaxKind k) {
    switch (k) {
        case SyntaxKind::IntLiteral:
        case SyntaxKind::LongLiteral:
        case SyntaxKind::FloatLiteral:
        case SyntaxKind::DoubleLiteral:
        case SyntaxKind::StringLiteral:
        case SyntaxKind::CharLiteral:
        case SyntaxKind::KwTrue:
        case SyntaxKind::KwFalse:
        case SyntaxKind::KwNull:
            return true;
        default:
            return false;
    }
}

static constexpr std::pair<SyntaxKind, std::string_view> KIND_NAMES[] = {
    {SyntaxKind::Whitespace,        "Whitespace"},
    {SyntaxKind::Newline,           "Newline"},
    {SyntaxKind::LineComment,       "LineComment"},
    {SyntaxKind::BlockComment,      "BlockComment"},
    {SyntaxKind::EndOfFile,         "EndOfFile"},
    {SyntaxKind::Invalid,           "Invalid"},
    {SyntaxKind::Missing,           "Missing"},
    {SyntaxKind::IntLiteral,        "IntLiteral"},
    {SyntaxKind::LongLiteral,       "LongLiteral"},
    {SyntaxKind::FloatLiteral,      "FloatLiteral"},
    {SyntaxKind::DoubleLiteral,     "DoubleLiteral"},
    {SyntaxKind::StringLiteral,     "StringLiteral"},
    {SyntaxKind::CharLiteral,       "CharLiteral"},
    {SyntaxKind::InterpStringStart, "InterpStringStart"},
    {SyntaxKind::InterpStringMid,   "InterpStringMid"},
    {SyntaxKind::InterpStringEnd,   "InterpStringEnd"},
    {SyntaxKind::Identifier,        "Identifier"},
    {SyntaxKind::LParen,            "LParen"},
    {SyntaxKind::RParen,            "RParen"},
    {SyntaxKind::LBrace,            "LBrace"},
    {SyntaxKind::RBrace,            "RBrace"},
    {SyntaxKind::LBracket,          "LBracket"},
    {SyntaxKind::RBracket,          "RBracket"},
    {SyntaxKind::Semi,              "Semi"},
    {SyntaxKind::Comma,             "Comma"},
    {SyntaxKind::Dot,               "Dot"},
    {SyntaxKind::Ellipsis,          "Ellipsis"},
    {SyntaxKind::Colon,             "Colon"},
    {SyntaxKind::Arrow,             "Arrow"},
    {SyntaxKind::At,                "At"},
    {SyntaxKind::Plus,              "Plus"},
    {SyntaxKind::Minus,             "Minus"},
    {SyntaxKind::Star,              "Star"},
    {SyntaxKind::Slash,             "Slash"},
    {SyntaxKind::Percent,           "Percent"},
    {SyntaxKind::Caret,             "Caret"},
    {SyntaxKind::Eq,                "Eq"},
    {SyntaxKind::Bang,              "Bang"},
    {SyntaxKind::Question,          "Question"},
    {SyntaxKind::QuestionDot,       "QuestionDot"},
    {SyntaxKind::EqEq,              "EqEq"},
    {SyntaxKind::NotEq,             "NotEq"},
    {SyntaxKind::Lt,                "Lt"},
    {SyntaxKind::Gt,                "Gt"},
    {SyntaxKind::LtEq,              "LtEq"},
    {SyntaxKind::GtEq,              "GtEq"},
    {SyntaxKind::AmpAmp,            "AmpAmp"},
    {SyntaxKind::PipePipe,          "PipePipe"},
    {SyntaxKind::Amp,               "Amp"},
    {SyntaxKind::Pipe,              "Pipe"},
    {SyntaxKind::PlusPlus,          "PlusPlus"},
    {SyntaxKind::MinusMinus,        "MinusMinus"},
    {SyntaxKind::LtLt,              "LtLt"},
    {SyntaxKind::GtGt,              "GtGt"},
    {SyntaxKind::GtGtGt,            "GtGtGt"},
    {SyntaxKind::PlusEq,            "PlusEq"},
    {SyntaxKind::MinusEq,           "MinusEq"},
    {SyntaxKind::StarEq,            "StarEq"},
    {SyntaxKind::SlashEq,           "SlashEq"},
    {SyntaxKind::PercentEq,         "PercentEq"},
    {SyntaxKind::AmpEq,             "AmpEq"},
    {SyntaxKind::PipeEq,            "PipeEq"},
    {SyntaxKind::CaretEq,           "CaretEq"},
    {SyntaxKind::LtLtEq,            "LtLtEq"},
    {SyntaxKind::GtGtEq,            "GtGtEq"},
    {SyntaxKind::GtGtGtEq,          "GtGtGtEq"},
    {SyntaxKind::KwBool,            "KwBool"},
    {SyntaxKind::KwByte,            "KwByte"},
    {SyntaxKind::KwShort,           "KwShort"},
    {SyntaxKind::KwUShort,          "KwUShort"},
    {SyntaxKind::KwInt,             "KwInt"},
    {SyntaxKind::KwUInt,            "KwUInt"},
    {SyntaxKind::KwLong,            "KwLong"},
    {SyntaxKind::KwULong,           "KwULong"},
    {SyntaxKind::KwFloat,           "KwFloat"},
    {SyntaxKind::KwDouble,          "KwDouble"},
    {SyntaxKind::KwDecimal,         "KwDecimal"},
    {SyntaxKind::KwChar,            "KwChar"},
    {SyntaxKind::KwString,          "KwString"},
    {SyntaxKind::KwVoid,            "KwVoid"},
    {SyntaxKind::KwAbstract,        "KwAbstract"},
    {SyntaxKind::KwAs,              "KwAs"},
    {SyntaxKind::KwAssert,          "KwAssert"},
    {SyntaxKind::KwBreak,           "KwBreak"},
    {SyntaxKind::KwCase,            "KwCase"},
    {SyntaxKind::KwCatch,           "KwCatch"},
    {SyntaxKind::KwClass,           "KwClass"},
    {SyntaxKind::KwComputed,        "KwComputed"},
    {SyntaxKind::KwConst,           "KwConst"},
    {SyntaxKind::KwContinue,        "KwContinue"},
    {SyntaxKind::KwDefault,         "KwDefault"},
    {SyntaxKind::KwDo,              "KwDo"},
    {SyntaxKind::KwElse,            "KwElse"},
    {SyntaxKind::KwEnum,            "KwEnum"},
    {SyntaxKind::KwExtends,         "KwExtends"},
    {SyntaxKind::KwExternal,        "KwExternal"},
    {SyntaxKind::KwFalse,           "KwFalse"},
    {SyntaxKind::KwFinal,           "KwFinal"},
    {SyntaxKind::KwFinally,         "KwFinally"},
    {SyntaxKind::KwFor,             "KwFor"},
    {SyntaxKind::KwFrom,            "KwFrom"},
    {SyntaxKind::KwGoto,            "KwGoto"},
    {SyntaxKind::KwIf,              "KwIf"},
    {SyntaxKind::KwImport,          "KwImport"},
    {SyntaxKind::KwIn,              "KwIn"},
    {SyntaxKind::KwLet,             "KwLet"},
    {SyntaxKind::KwNew,             "KwNew"},
    {SyntaxKind::KwNull,            "KwNull"},
    {SyntaxKind::KwOut,             "KwOut"},
    {SyntaxKind::KwOverride,        "KwOverride"},
    {SyntaxKind::KwPackage,         "KwPackage"},
    {SyntaxKind::KwPrivate,         "KwPrivate"},
    {SyntaxKind::KwProtected,       "KwProtected"},
    {SyntaxKind::KwPublic,          "KwPublic"},
    {SyntaxKind::KwRethrow,         "KwRethrow"},
    {SyntaxKind::KwReturn,          "KwReturn"},
    {SyntaxKind::KwStatic,          "KwStatic"},
    {SyntaxKind::KwStruct,          "KwStruct"},
    {SyntaxKind::KwSuper,           "KwSuper"},
    {SyntaxKind::KwSwitch,          "KwSwitch"},
    {SyntaxKind::KwTest,            "KwTest"},
    {SyntaxKind::KwThis,            "KwThis"},
    {SyntaxKind::KwThrow,           "KwThrow"},
    {SyntaxKind::KwThrows,          "KwThrows"},
    {SyntaxKind::KwTrue,            "KwTrue"},
    {SyntaxKind::KwTry,             "KwTry"},
    {SyntaxKind::KwType,            "KwType"},
    {SyntaxKind::KwWeak,            "KwWeak"},
    {SyntaxKind::KwWhile,           "KwWhile"},
    {SyntaxKind::SourceFile,        "SourceFile"},
    {SyntaxKind::FuncDecl,          "FuncDecl"},
    {SyntaxKind::ParamList,         "ParamList"},
    {SyntaxKind::Parameter,         "Parameter"},
    {SyntaxKind::DefaultValue,      "DefaultValue"},
    {SyntaxKind::ReturnType,        "ReturnType"},
    {SyntaxKind::ThrowsClause,      "ThrowsClause"},
    {SyntaxKind::CatchClause,       "CatchClause"},
    {SyntaxKind::StructDecl,        "StructDecl"},
    {SyntaxKind::ClassDecl,         "ClassDecl"},
    {SyntaxKind::FieldDecl,         "FieldDecl"},
    {SyntaxKind::MemberList,        "MemberList"},
    {SyntaxKind::VisibilityModifier, "VisibilityModifier"},
    {SyntaxKind::ImportDecl,        "ImportDecl"},
    {SyntaxKind::ImportPath,        "ImportPath"},
    {SyntaxKind::ExternalTypeDecl,  "ExternalTypeDecl"},
    {SyntaxKind::ExternalBlock,     "ExternalBlock"},
    {SyntaxKind::LibrarySpec,       "LibrarySpec"},
    {SyntaxKind::ExternalFuncDecl,  "ExternalFuncDecl"},
    {SyntaxKind::EnumDecl,          "EnumDecl"},
    {SyntaxKind::EnumMember,        "EnumMember"},
    {SyntaxKind::TypeParamList,     "TypeParamList"},
    {SyntaxKind::TypeParam,         "TypeParam"},
    {SyntaxKind::TestDecl,          "TestDecl"},
    {SyntaxKind::TypeRef,           "TypeRef"},
    {SyntaxKind::OptionalType,      "OptionalType"},
    {SyntaxKind::TypeArgList,       "TypeArgList"},
    {SyntaxKind::Block,             "Block"},
    {SyntaxKind::LetStmt,           "LetStmt"},
    {SyntaxKind::TypedVarDecl,      "TypedVarDecl"},
    {SyntaxKind::IfStmt,            "IfStmt"},
    {SyntaxKind::ElseClause,        "ElseClause"},
    {SyntaxKind::WhileStmt,         "WhileStmt"},
    {SyntaxKind::ForStmt,           "ForStmt"},
    {SyntaxKind::ForEachStmt,       "ForEachStmt"},
    {SyntaxKind::ForUpdate,         "ForUpdate"},
    {SyntaxKind::BreakStmt,         "BreakStmt"},
    {SyntaxKind::ContinueStmt,      "ContinueStmt"},
    {SyntaxKind::ReturnStmt,        "ReturnStmt"},
    {SyntaxKind::ExprStmt,          "ExprStmt"},
    {SyntaxKind::ThrowStmt,         "ThrowStmt"},
    {SyntaxKind::RethrowStmt,       "RethrowStmt"},
    {SyntaxKind::SwitchStmt,        "SwitchStmt"},
    {SyntaxKind::SwitchArm,         "SwitchArm"},
    {SyntaxKind::LiteralExpr,       "LiteralExpr"},
    {SyntaxKind::IdentExpr,         "IdentExpr"},
    {SyntaxKind::ThisExpr,          "ThisExpr"},
    {SyntaxKind::SuperExpr,         "SuperExpr"},
    {SyntaxKind::BinaryExpr,        "BinaryExpr"},
    {SyntaxKind::UnaryExpr,         "UnaryExpr"},
    {SyntaxKind::PrefixExpr,        "PrefixExpr"},
    {SyntaxKind::PostfixExpr,       "PostfixExpr"},
    {SyntaxKind::CallExpr,          "CallExpr"},
    {SyntaxKind::ArgList,           "ArgList"},
    {SyntaxKind::OutArgument,       "OutArgument"},
    {SyntaxKind::MemberExpr,        "MemberExpr"},
    {SyntaxKind::SafeMemberExpr,    "SafeMemberExpr"},
    {SyntaxKind::SubscriptExpr,     "SubscriptExpr"},
    {SyntaxKind::SafeSubscriptExpr, "SafeSubscriptExpr"},
    {SyntaxKind::CastExpr,          "CastExpr"},
    {SyntaxKind::AssignExpr,        "AssignExpr"},
    {SyntaxKind::TernaryExpr,       "TernaryExpr"},
    {SyntaxKind::NullCoalesceExpr,  "NullCoalesceExpr"},
    {SyntaxKind::NewExpr,           "NewExpr"},
    {SyntaxKind::ParenExpr,         "ParenExpr"},
    {SyntaxKind::ArrayLiteralExpr,  "ArrayLiteralExpr"},
    {SyntaxKind::InterpStringExpr,  "InterpStringExpr"},
    {SyntaxKind::TryExpr,           "TryExpr"},
    {SyntaxKind::SwitchExpr,        "SwitchExpr"},
    {SyntaxKind::Error,             "Error"},
};

std::string_view kindName(SyntaxKind k) {
    for (const auto& [kind, name] : KIND_NAMES) {
        if (kind == k) return name;
    }
    return "<unknown>";
}

static constexpr std::pair<std::u16string_view, SyntaxKind> KEYWORD_TABLE[] = {
    {u"bool",      SyntaxKind::KwBool},
    {u"byte",      SyntaxKind::KwByte},
    {u"short",     SyntaxKind::KwShort},
    {u"ushort",    SyntaxKind::KwUShort},
    {u"int",       SyntaxKind::KwInt},
    {u"uint",      SyntaxKind::KwUInt},
    {u"long",      SyntaxKind::KwLong},
    {u"ulong",     SyntaxKind::KwULong},
    {u"float",     SyntaxKind::KwFloat},
    {u"double",    SyntaxKind::KwDouble},
    {u"decimal",   SyntaxKind::KwDecimal},
    {u"char",      SyntaxKind::KwChar},
    {u"string",    SyntaxKind::KwString},
    {u"void",      SyntaxKind::KwVoid},
    {u"abstract",  SyntaxKind::KwAbstract},
    {u"as",        SyntaxKind::KwAs},
    {u"assert",    SyntaxKind::KwAssert},
    {u"break",     SyntaxKind::KwBreak},
    {u"case",      SyntaxKind::KwCase},
    {u"catch",     SyntaxKind::KwCatch},
    {u"class",     SyntaxKind::KwClass},
    {u"computed",  SyntaxKind::KwComputed},
    {u"const",     SyntaxKind::KwConst},
    {u"continue",  SyntaxKind::KwContinue},
    {u"default",   SyntaxKind::KwDefault},
    {u"do",        SyntaxKind::KwDo},
    {u"else",      SyntaxKind::KwElse},
    {u"enum",      SyntaxKind::KwEnum},
    {u"extends",   SyntaxKind::KwExtends},
    {u"external",  SyntaxKind::KwExternal},
    {u"false",     SyntaxKind::KwFalse},
    {u"final",     SyntaxKind::KwFinal},
    {u"finally",   SyntaxKind::KwFinally},
    {u"for",       SyntaxKind::KwFor},
    {u"from",      SyntaxKind::KwFrom},
    {u"goto",      SyntaxKind::KwGoto},
    {u"if",        SyntaxKind::KwIf},
    {u"import",    SyntaxKind::KwImport},
    {u"let",       SyntaxKind::KwLet},
    {u"new",       SyntaxKind::KwNew},
    {u"null",      SyntaxKind::KwNull},
    {u"override",  SyntaxKind::KwOverride},
    {u"package",   SyntaxKind::KwPackage},
    {u"private",   SyntaxKind::KwPrivate},
    {u"protected", SyntaxKind::KwProtected},
    {u"public",    SyntaxKind::KwPublic},
    {u"rethrow",   SyntaxKind::KwRethrow},
    {u"return",    SyntaxKind::KwReturn},
    {u"static",    SyntaxKind::KwStatic},
    {u"struct",    SyntaxKind::KwStruct},
    {u"super",     SyntaxKind::KwSuper},
    {u"switch",    SyntaxKind::KwSwitch},
    {u"this",      SyntaxKind::KwThis},
    {u"throw",     SyntaxKind::KwThrow},
    {u"throws",    SyntaxKind::KwThrows},
    {u"true",      SyntaxKind::KwTrue},
    {u"try",       SyntaxKind::KwTry},
    {u"type",      SyntaxKind::KwType},
    {u"weak",      SyntaxKind::KwWeak},
    {u"while",     SyntaxKind::KwWhile},
};

SyntaxKind keywordKindFromText(std::u16string_view text) {
    for (const auto& [kw, kind] : KEYWORD_TABLE) {
        if (kw == text) return kind;
    }
    return SyntaxKind::Identifier;
}
