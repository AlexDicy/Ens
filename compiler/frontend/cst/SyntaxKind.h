#pragma once
#include <cstdint>
#include <string_view>

enum class SyntaxKind : uint16_t {
    // === Trivia ===
    Whitespace,
    Newline,
    LineComment,
    BlockComment,

    // === Special tokens ===
    EndOfFile,
    Invalid,         // tokenizer-level lex error
    Missing,         // parser-inserted placeholder for an expected-but-absent token

    // === Literals ===
    IntLiteral,
    LongLiteral,
    FloatLiteral,
    DoubleLiteral,
    StringLiteral,
    CharLiteral,

    // === Identifier ===
    Identifier,

    // === Punctuation ===
    LParen, RParen, LBrace, RBrace, LBracket, RBracket,
    Semi, Comma, Dot, Ellipsis, Colon, Arrow, At,
    Plus, Minus, Star, Slash, Percent, Caret,
    Eq, Bang, Question, QuestionDot,
    EqEq, NotEq, Lt, Gt, LtEq, GtEq,
    AmpAmp, PipePipe,
    Amp, Pipe,
    PlusPlus, MinusMinus,
    LtLt, GtGt, GtGtGt,
    PlusEq, MinusEq, StarEq, SlashEq, PercentEq,
    AmpEq, PipeEq, CaretEq,
    LtLtEq, GtGtEq, GtGtGtEq,

    // === Keywords ===
    KwBool, KwByte, KwShort, KwUShort, KwInt, KwUInt, KwLong, KwULong,
    KwFloat, KwDouble, KwDecimal, KwChar, KwString, KwVoid,
    KwAbstract, KwAs, KwAssert, KwBreak, KwCase, KwCatch, KwClass, KwComputed, KwConst, KwContinue,
    KwDefault, KwDo, KwElse, KwEnum, KwExtends, KwExternal, KwFalse, KwFinal, KwFinally,
    KwFor, KwFrom, KwGoto, KwIf, KwImport, KwLet, KwNew, KwNull, KwOut, KwOverride, KwPackage,
    KwPrivate, KwProtected, KwPublic, KwRethrow, KwReturn, KwStatic, KwStruct,
    KwSuper, KwSwitch, KwThis, KwThrow, KwThrows, KwTrue, KwTry, KwType, KwWeak, KwWhile,

    // ============================================================
    // Marker: everything after this point is a non-terminal node.
    // ============================================================
    FIRST_NODE_KIND,

    // === Top-level ===
    SourceFile,

    // === Declarations ===
    FuncDecl,
    ParamList,
    Parameter,           // includes regular and this-field parameters
    DefaultValue,        // = expr suffix on a parameter
    ReturnType,          // -> Type
    StructDecl,
    ClassDecl,
    FieldDecl,
    MemberList,
    VisibilityModifier,
    ImportDecl,
    ImportPath,
    ExternalTypeDecl,
    ExternalBlock,
    LibrarySpec,
    ExternalFuncDecl,

    // === Type expressions ===
    TypeRef,             // identifier (or primitive keyword) optionally followed by ?
    OptionalType,        // wraps TypeRef with trailing ?

    // === Statements ===
    Block,
    LetStmt,
    TypedVarDecl,
    IfStmt,
    ElseClause,
    WhileStmt,
    ReturnStmt,
    ExprStmt,

    // === Expressions ===
    LiteralExpr,         // wraps any literal token
    IdentExpr,
    ThisExpr,
    SuperExpr,
    BinaryExpr,
    UnaryExpr,
    PrefixExpr,
    PostfixExpr,
    CallExpr,
    ArgList,
    OutArgument,
    MemberExpr,
    SafeMemberExpr,
    SubscriptExpr,
    SafeSubscriptExpr,
    CastExpr,
    AssignExpr,
    TernaryExpr,
    NewExpr,
    ParenExpr,
    ArrayLiteralExpr,

    // === Recovery ===
    Error,               // wraps unexpected/skipped tokens during recovery

    LAST_KIND
};

bool isToken(SyntaxKind k);
bool isTrivia(SyntaxKind k);
bool isNode(SyntaxKind k);
bool isKeyword(SyntaxKind k);
bool isLiteral(SyntaxKind k);

std::string_view kindName(SyntaxKind k);

SyntaxKind keywordKindFromText(std::u16string_view text);
