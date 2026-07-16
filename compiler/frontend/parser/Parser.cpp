#include "Parser.h"

#include <algorithm>
#include "../diagnostics/Diagnostic.h"
#include "../diagnostics/DiagnosticSink.h"

Parser::Parser(std::u16string_view src, DiagnosticSink& s)
    : source(src), sink(s) {
    Tokenizer tokenizer(source, sink);
    while (true) {
        LexedToken t = tokenizer.next();
        bool eof = (t.kind == SyntaxKind::EndOfFile);
        tokens.push_back(std::move(t));
        if (eof) break;
    }
    // Position `current` at the first non-trivia token.
    while (current < tokens.size() && isTrivia(tokens[current].kind)) current++;
}

SyntaxKind Parser::kindAt() const {
    return current < tokens.size() ? tokens[current].kind : SyntaxKind::EndOfFile;
}

const LexedToken& Parser::tokenAt() const {
    return tokens[current];
}

bool Parser::at(SyntaxKind k) const {
    return kindAt() == k;
}

bool Parser::atAny(std::initializer_list<SyntaxKind> kinds) const {
    SyntaxKind k = kindAt();
    for (auto x : kinds) if (x == k) return true;
    return false;
}

bool Parser::atEnd() const {
    return kindAt() == SyntaxKind::EndOfFile;
}

SyntaxKind Parser::peekKind(size_t n) const {
    size_t idx = current;
    while (true) {
        if (idx >= tokens.size()) return SyntaxKind::EndOfFile;
        if (!isTrivia(tokens[idx].kind)) {
            if (n == 0) return tokens[idx].kind;
            n--;
        }
        idx++;
    }
}

void Parser::bumpAs(SyntaxKind kind) {
    while (nextToEmit < tokens.size() && nextToEmit < current) {
        const auto& t = tokens[nextToEmit];
        builder.token(t.kind, t.text);
        nextToEmit++;
    }
    if (current < tokens.size() && !atEnd()) {
        const auto& t = tokens[current];
        builder.token(kind, t.text);
        nextToEmit = current + 1;
        current++;
        while (current < tokens.size() && isTrivia(tokens[current].kind)) current++;
    }
}

void Parser::bump() { bumpAs(kindAt()); }

// `out` is a contextual keyword: an ordinary identifier everywhere except in the FFI
// parameter / argument modifier position, recognized at the use sites below.
bool Parser::atContextualOut() const {
    return kindAt() == SyntaxKind::Identifier && tokenAt().text == u"out";
}

// `test` is a contextual keyword: an ordinary identifier everywhere except at
// the top level when followed by a string literal, which starts a test declaration.
bool Parser::atContextualTest() const {
    return kindAt() == SyntaxKind::Identifier && tokenAt().text == u"test";
}

// `in` is a contextual keyword, an ordinary identifier everywhere except between
// a foreach binding and its iterable.
bool Parser::atContextualIn() const {
    return kindAt() == SyntaxKind::Identifier && tokenAt().text == u"in";
}

bool Parser::peekIsContextualIn(size_t n) const {
    size_t idx = current;
    while (true) {
        if (idx >= tokens.size()) return false;
        if (!isTrivia(tokens[idx].kind)) {
            if (n == 0) {
                return tokens[idx].kind == SyntaxKind::Identifier && tokens[idx].text == u"in";
            }
            n--;
        }
        idx++;
    }
}

bool Parser::eat(SyntaxKind k) {
    if (!at(k)) return false;
    bump();
    return true;
}

void Parser::expect(SyntaxKind k, const char* what) {
    if (at(k)) {
        bump();
    } else {
        emitMissing(k, what);
    }
}

void Parser::emitMissing(SyntaxKind /*expectedKind*/, const char* what) {
    // Flush trivia preceding the current cursor position so the missing-token
    // marker lands at the right offset in the tree.
    while (nextToEmit < tokens.size() && nextToEmit < current) {
        const auto& t = tokens[nextToEmit];
        builder.token(t.kind, t.text);
        nextToEmit++;
    }
    builder.token(SyntaxKind::Missing, std::u16string{});
    reportAtCurrent(std::string("Expected ") + what);
}

void Parser::reportAtCurrent(std::string message) {
    int line, column, length;
    if (current < tokens.size()) {
        line = tokens[current].line;
        column = tokens[current].column;
        length = std::max<int>(1, static_cast<int>(tokens[current].text.size()));
    } else {
        line = 1;
        column = 1;
        length = 1;
    }
    sink.error({line, column, length}, std::move(message));
}

void Parser::recoverTo(std::initializer_list<SyntaxKind> syncSet) {
    if (atEnd()) return;
    builder.startNode(SyntaxKind::Error);
    bool any = false;
    while (!atEnd()) {
        SyntaxKind k = kindAt();
        bool inSet = false;
        for (auto x : syncSet) if (x == k) { inSet = true; break; }
        if (inSet) break;
        bump();
        any = true;
    }
    if (!any) {
        // Avoid an empty Error node; finish it anyway since startNode was called.
    }
    builder.finishNode();
}

// =================================================================
// Top level
// =================================================================

GreenElementPtr Parser::parseSourceFile() {
    builder.startNode(SyntaxKind::SourceFile);
    while (!atEnd()) {
        size_t before = current;
        parseTopLevel();
        if (current == before) {
            // Defensive: if no progress was made, force advance to avoid infinite loop.
            reportAtCurrent("Unexpected token at top level");
            recoverTo({SyntaxKind::KwImport, SyntaxKind::KwStruct, SyntaxKind::KwClass,
                       SyntaxKind::KwInterface,
                       SyntaxKind::KwPrivate, SyntaxKind::KwProtected, SyntaxKind::KwPublic,
                       SyntaxKind::Identifier, SyntaxKind::Semi, SyntaxKind::EndOfFile});
            if (current == before && !atEnd()) bump();
        }
    }
    // Flush any trailing trivia at end of file.
    while (nextToEmit < tokens.size() && tokens[nextToEmit].kind != SyntaxKind::EndOfFile) {
        const auto& t = tokens[nextToEmit];
        builder.token(t.kind, t.text);
        nextToEmit++;
    }
    builder.finishNode();
    return builder.build();
}

void Parser::parseTopLevel() {
    if (at(SyntaxKind::KwImport)) {
        parseImportDecl();
        return;
    }

    bool hasVisibility = atAny({SyntaxKind::KwPrivate, SyntaxKind::KwProtected, SyntaxKind::KwPublic});

    if (at(SyntaxKind::KwExternal) ||
        peekKind(hasVisibility ? 1 : 0) == SyntaxKind::KwExternal) {
        parseExternalDecl();
        return;
    }
    // Skip leading declaration modifiers (visibility + abstract/final/sealed) to classify struct vs class.
    int declMods = 0;
    while (true) {
        SyntaxKind k = peekKind(declMods);
        if (k == SyntaxKind::KwPrivate || k == SyntaxKind::KwProtected || k == SyntaxKind::KwPublic ||
            k == SyntaxKind::KwAbstract || k == SyntaxKind::KwFinal || k == SyntaxKind::KwSealed) {
            declMods++;
            continue;
        }
        break;
    }
    if (peekKind(declMods) == SyntaxKind::KwStruct) {
        parseStructOrClassDecl(SyntaxKind::StructDecl, SyntaxKind::KwStruct);
        return;
    }
    if (peekKind(declMods) == SyntaxKind::KwClass) {
        parseStructOrClassDecl(SyntaxKind::ClassDecl, SyntaxKind::KwClass);
        return;
    }
    if (peekKind(declMods) == SyntaxKind::KwInterface) {
        parseInterfaceDecl();
        return;
    }
    if (peekKind(declMods) == SyntaxKind::KwEnum) {
        parseEnumDecl();
        return;
    }

    if (atContextualTest() &&
        (peekKind(1) == SyntaxKind::StringLiteral || peekKind(1) == SyntaxKind::InterpStringStart)) {
        parseTestDecl();
        return;
    }

    // Check function decl first, then typed var decl.
    if (looksLikeFuncDecl(/*allowShorthand=*/false)) {
        parseFuncDecl();
        return;
    }
    if (looksLikeTypedVarDecl()) {
        parseTypedVarDeclStmt();
        return;
    }

    // Fallback: treat as statement to recover from misplaced code.
    if (atEnd()) return;
    parseStatement();
}

void Parser::parseVisibilityModifier() {
    if (!atAny({SyntaxKind::KwPrivate, SyntaxKind::KwProtected, SyntaxKind::KwPublic})) return;
    builder.startNode(SyntaxKind::VisibilityModifier);
    bump();
    builder.finishNode();
}

// =================================================================
// Import declarations
// =================================================================

void Parser::parseImportDecl() {
    builder.startNode(SyntaxKind::ImportDecl);
    expect(SyntaxKind::KwImport, "'import'");

    // Optional alias: `import Identifier from path;`
    if (peekKind(0) == SyntaxKind::Identifier && peekKind(1) == SyntaxKind::KwFrom) {
        bump();  // alias identifier
        bump();  // 'from'
    }

    parseImportPath();
    expect(SyntaxKind::Semi, "';' after import");
    builder.finishNode();
}

void Parser::parseImportPath() {
    builder.startNode(SyntaxKind::ImportPath);
    eat(SyntaxKind::At);  // optional '@' prefix for package imports
    expect(SyntaxKind::Identifier, "path segment");
    while (at(SyntaxKind::Dot) && peekKind(1) == SyntaxKind::Identifier) {
        bump();  // '.'
        bump();  // identifier
    }
    builder.finishNode();
}

// =================================================================
// External (FFI) declarations
// =================================================================

void Parser::parseExternalDecl() {
    // Lookahead: 'external' 'type' IDENT ';'     -> ExternalTypeDecl
    //            'external' 'from' STRING { ... } -> ExternalBlock
    size_t externalIdx = (kindAt() == SyntaxKind::KwExternal) ? 0 : 1;
    SyntaxKind afterExternal = peekKind(externalIdx + 1);
    if (afterExternal == SyntaxKind::KwType) {
        parseExternalTypeDecl();
    } else {
        parseExternalBlock();
    }
}

void Parser::parseExternalTypeDecl() {
    builder.startNode(SyntaxKind::ExternalTypeDecl);
    parseVisibilityModifier();
    expect(SyntaxKind::KwExternal, "'external'");
    expect(SyntaxKind::KwType, "'type'");
    expect(SyntaxKind::Identifier, "external type name");
    expect(SyntaxKind::Semi, "';' after external type declaration");
    builder.finishNode();
}

void Parser::parseExternalBlock() {
    builder.startNode(SyntaxKind::ExternalBlock);
    parseVisibilityModifier();
    expect(SyntaxKind::KwExternal, "'external'");
    expect(SyntaxKind::KwFrom, "'from' after 'external'");
    {
        builder.startNode(SyntaxKind::LibrarySpec);
        expect(SyntaxKind::StringLiteral, "library name string");
        builder.finishNode();
    }
    expect(SyntaxKind::LBrace, "'{' to begin external block");
    while (!at(SyntaxKind::RBrace) && !atEnd()) {
        size_t before = current;
        parseExternalFuncDecl();
        if (current == before) {
            reportAtCurrent("Unexpected token in external block");
            recoverTo({SyntaxKind::RBrace, SyntaxKind::Semi, SyntaxKind::EndOfFile});
            eat(SyntaxKind::Semi);
            if (current == before && !atEnd()) bump();
        }
    }
    expect(SyntaxKind::RBrace, "'}' to close external block");
    builder.finishNode();
}

void Parser::parseExternalFuncDecl() {
    builder.startNode(SyntaxKind::ExternalFuncDecl);
    expect(SyntaxKind::Identifier, "external function name");
    expect(SyntaxKind::LParen, "'(' after external function name");
    parseParamList();
    expect(SyntaxKind::RParen, "')' to close parameter list");
    if (at(SyntaxKind::Arrow)) parseReturnType();
    expect(SyntaxKind::Semi, "';' after external function declaration");
    builder.finishNode();
}

// =================================================================
// Function declarations
// =================================================================

bool Parser::looksLikeFuncDecl(bool allowShorthand) const {
    size_t idx = current;
    // Skip an optional visibility modifier.
    if (idx < tokens.size() && (tokens[idx].kind == SyntaxKind::KwPrivate ||
                                tokens[idx].kind == SyntaxKind::KwProtected ||
                                tokens[idx].kind == SyntaxKind::KwPublic)) {
        idx++;
        while (idx < tokens.size() && isTrivia(tokens[idx].kind)) idx++;
    }
    // Skip optional method modifiers (override / final / abstract).
    while (idx < tokens.size() && (tokens[idx].kind == SyntaxKind::KwOverride ||
                                   tokens[idx].kind == SyntaxKind::KwFinal ||
                                   tokens[idx].kind == SyntaxKind::KwAbstract)) {
        idx++;
        while (idx < tokens.size() && isTrivia(tokens[idx].kind)) idx++;
    }
    if (idx >= tokens.size() || tokens[idx].kind != SyntaxKind::Identifier) return false;
    idx++;
    while (idx < tokens.size() && isTrivia(tokens[idx].kind)) idx++;
    idx = skipAnglesRaw(idx);
    while (idx < tokens.size() && isTrivia(tokens[idx].kind)) idx++;
    if (idx >= tokens.size() || tokens[idx].kind != SyntaxKind::LParen) return false;

    // Walk balanced parens.
    int depth = 1;
    idx++;
    while (idx < tokens.size() && depth > 0) {
        SyntaxKind k = tokens[idx].kind;
        if (k == SyntaxKind::LParen) depth++;
        else if (k == SyntaxKind::RParen) depth--;
        else if (k == SyntaxKind::EndOfFile) return false;
        idx++;
    }
    if (depth != 0) return false;
    while (idx < tokens.size() && isTrivia(tokens[idx].kind)) idx++;
    if (idx >= tokens.size()) return false;
    SyntaxKind after = tokens[idx].kind;
    if (after == SyntaxKind::Arrow || after == SyntaxKind::LBrace ||
        after == SyntaxKind::KwThrows) return true;
    return allowShorthand && after == SyntaxKind::Semi;
}

void Parser::parseFuncDecl() {
    builder.startNode(SyntaxKind::FuncDecl);
    parseVisibilityModifier();
    while (at(SyntaxKind::KwOverride) || at(SyntaxKind::KwFinal) || at(SyntaxKind::KwAbstract)) {
        bump();  // method modifier; analyzer validates context
    }
    if (at(SyntaxKind::KwConstructor) || at(SyntaxKind::KwDestructor)) {
        bump();  // constructor/destructor introducer; no name follows
    } else if (isKeyword(kindAt())) {
        std::string word;
        for (char16_t c : tokenAt().text) word.push_back(c < 128 ? static_cast<char>(c) : '?');
        reportAtCurrent("'" + word + "' is a keyword and cannot be used as a method name");
        bumpAs(SyntaxKind::Identifier);
    } else {
        expect(SyntaxKind::Identifier, "function name");
    }
    if (at(SyntaxKind::Lt)) parseTypeParamList();
    expect(SyntaxKind::LParen, "'(' after function name");
    parseParamList();
    expect(SyntaxKind::RParen, "')' after parameters");
    if (at(SyntaxKind::Arrow)) parseReturnType();
    if (at(SyntaxKind::KwThrows)) parseThrowsClause();
    if (at(SyntaxKind::LBrace)) {
        parseBlock();
        while (at(SyntaxKind::KwCatch)) parseCatchClause();
        if (at(SyntaxKind::KwFinally)) {
            reportAtCurrent("'finally' is not supported");
            builder.startNode(SyntaxKind::Error);
            bump();  // 'finally'
            if (at(SyntaxKind::LBrace)) parseBlock();
            builder.finishNode();
            while (at(SyntaxKind::KwCatch)) parseCatchClause();
        }
    } else if (eat(SyntaxKind::Semi)) {
        // shorthand body
    } else {
        emitMissing(SyntaxKind::LBrace, "'{' or ';' for function body");
        recoverTo({SyntaxKind::RBrace, SyntaxKind::Semi,
                   SyntaxKind::KwStruct, SyntaxKind::KwClass,
                   SyntaxKind::KwPrivate, SyntaxKind::KwProtected, SyntaxKind::KwPublic,
                   SyntaxKind::EndOfFile});
        eat(SyntaxKind::Semi);
    }
    builder.finishNode();
}

void Parser::parseTestDecl() {
    builder.startNode(SyntaxKind::TestDecl);
    bumpAs(SyntaxKind::KwTest);
    if (at(SyntaxKind::StringLiteral)) {
        bump();
    } else if (at(SyntaxKind::InterpStringStart)) {
        reportAtCurrent("A test description must be a plain string without interpolation");
        builder.startNode(SyntaxKind::Error);
        while (!at(SyntaxKind::LBrace) && !atEnd()) bump();
        builder.finishNode();
    } else {
        emitMissing(SyntaxKind::StringLiteral, "test description string");
    }
    if (at(SyntaxKind::LBrace)) {
        parseBlock();
    } else {
        emitMissing(SyntaxKind::LBrace, "'{' for the test body");
        recoverTo({SyntaxKind::RBrace, SyntaxKind::Semi,
                   SyntaxKind::KwStruct, SyntaxKind::KwClass,
                   SyntaxKind::KwPrivate, SyntaxKind::KwProtected, SyntaxKind::KwPublic,
                   SyntaxKind::EndOfFile});
        eat(SyntaxKind::Semi);
    }
    builder.finishNode();
}

void Parser::parseParamList() {
    builder.startNode(SyntaxKind::ParamList);
    if (!at(SyntaxKind::RParen) && !atEnd()) {
        parseParameter();
        while (eat(SyntaxKind::Comma)) {
            parseParameter();
        }
    }
    builder.finishNode();
}

void Parser::parseParameter() {
    builder.startNode(SyntaxKind::Parameter);
    if (eat(SyntaxKind::KwThis)) {
        expect(SyntaxKind::Dot, "'.' after 'this' in parameter");
        expect(SyntaxKind::Identifier, "field name after 'this.'");
    } else {
        // optional 'out' modifier (followed by the parameter type); analyzer enforces context
        if (atContextualOut() && isTypeStart(peekKind(1))) bumpAs(SyntaxKind::KwOut);
        if (isTypeStart(kindAt())) {
            parseType();
        } else {
            emitMissing(SyntaxKind::Identifier, "parameter type");
        }
        // A keyword in the name slot (e.g. `string from`) would otherwise cascade
        // into a stream of unrelated errors; report it once and recover by taking
        // the keyword as the name, mirroring keyword-named method recovery.
        if (isKeyword(kindAt())) {
            std::string word;
            for (char16_t c : tokenAt().text) word.push_back(c < 128 ? static_cast<char>(c) : '?');
            reportAtCurrent("'" + word + "' is a keyword and cannot be used as a parameter name; "
                "choose a different name such as 'source'");
            bumpAs(SyntaxKind::Identifier);
        } else {
            expect(SyntaxKind::Identifier, "parameter name");
        }
    }
    if (at(SyntaxKind::Eq)) parseDefaultValue();
    builder.finishNode();
}

void Parser::parseDefaultValue() {
    builder.startNode(SyntaxKind::DefaultValue);
    bump();  // =
    parseExpression();
    builder.finishNode();
}

void Parser::parseReturnType() {
    builder.startNode(SyntaxKind::ReturnType);
    expect(SyntaxKind::Arrow, "'->'");
    if (isTypeStart(kindAt())) parseType();
    else emitMissing(SyntaxKind::Identifier, "return type");
    builder.finishNode();
}

void Parser::parseThrowsClause() {
    builder.startNode(SyntaxKind::ThrowsClause);
    expect(SyntaxKind::KwThrows, "'throws'");
    if (isTypeStart(kindAt())) {
        parseType();
        while (eat(SyntaxKind::Comma)) {
            if (isTypeStart(kindAt())) parseType();
            else emitMissing(SyntaxKind::Identifier, "exception type after ','");
        }
    }
    builder.finishNode();
}

void Parser::parseCatchClause() {
    builder.startNode(SyntaxKind::CatchClause);
    expect(SyntaxKind::KwCatch, "'catch'");
    expect(SyntaxKind::LParen, "'(' after 'catch'");
    if (isTypeStart(kindAt())) parseType();
    else emitMissing(SyntaxKind::Identifier, "exception type in catch clause");
    expect(SyntaxKind::Identifier, "exception variable name");
    expect(SyntaxKind::RParen, "')' after catch variable");
    if (at(SyntaxKind::LBrace)) {
        parseBlock();
    } else {
        emitMissing(SyntaxKind::LBrace, "'{' for catch body");
        recoverTo({SyntaxKind::KwCatch, SyntaxKind::KwFinally, SyntaxKind::RBrace,
                   SyntaxKind::KwStruct, SyntaxKind::KwClass,
                   SyntaxKind::KwPrivate, SyntaxKind::KwProtected, SyntaxKind::KwPublic,
                   SyntaxKind::EndOfFile});
    }
    builder.finishNode();
}

// =================================================================
// Struct / class declarations
// =================================================================

void Parser::parseStructOrClassDecl(SyntaxKind nodeKind, SyntaxKind keywordKind) {
    bool isClass = (keywordKind == SyntaxKind::KwClass);
    builder.startNode(nodeKind);
    parseVisibilityModifier();
    while (at(SyntaxKind::KwAbstract) || at(SyntaxKind::KwFinal) || at(SyntaxKind::KwSealed)) {
        if (!isClass) reportAtCurrent("'abstract', 'final', and 'sealed' are only allowed on classes");
        bump();
    }
    expect(keywordKind, keywordKind == SyntaxKind::KwStruct ? "'struct'" : "'class'");
    expect(SyntaxKind::Identifier, "name");
    if (at(SyntaxKind::Lt)) parseTypeParamList();
    if (at(SyntaxKind::KwExtends)) {
        if (!isClass) reportAtCurrent("Only classes can use 'extends'; structs do not support inheritance");
        bump();  // 'extends'
        expect(SyntaxKind::Identifier, "base class name after 'extends'");
        if (at(SyntaxKind::Lt)) parseTypeArgList();
    }
    if (at(SyntaxKind::KwImplements)) {
        if (!isClass) reportAtCurrent("Structs cannot implement interfaces; only classes can use 'implements'");
        parseImplementsClause();
    }
    expect(SyntaxKind::LBrace, "'{'");
    builder.startNode(SyntaxKind::MemberList);
    while (!at(SyntaxKind::RBrace) && !atEnd()) {
        size_t before = current;
        parseStructOrClassMember();
        if (current == before) {
            reportAtCurrent("Unexpected token in member list");
            recoverTo({SyntaxKind::RBrace, SyntaxKind::Semi, SyntaxKind::EndOfFile});
            eat(SyntaxKind::Semi);
            if (current == before && !atEnd()) bump();
        }
    }
    builder.finishNode();
    expect(SyntaxKind::RBrace, "'}'");
    builder.finishNode();
}

void Parser::parseImplementsClause() {
    builder.startNode(SyntaxKind::ImplementsClause);
    expect(SyntaxKind::KwImplements, "'implements'");
    if (isTypeStart(kindAt())) parseType();
    else emitMissing(SyntaxKind::Identifier, "interface name after 'implements'");
    while (eat(SyntaxKind::Comma)) {
        if (isTypeStart(kindAt())) parseType();
        else emitMissing(SyntaxKind::Identifier, "interface name after ','");
    }
    builder.finishNode();
}

void Parser::parseInterfaceDecl() {
    builder.startNode(SyntaxKind::InterfaceDecl);
    parseVisibilityModifier();
    while (at(SyntaxKind::KwAbstract) || at(SyntaxKind::KwFinal) || at(SyntaxKind::KwSealed)) {
        reportAtCurrent("'abstract', 'final', and 'sealed' are not allowed on an interface");
        bump();
    }
    expect(SyntaxKind::KwInterface, "'interface'");
    expect(SyntaxKind::Identifier, "interface name");
    if (at(SyntaxKind::Lt)) parseTypeParamList();
    if (at(SyntaxKind::KwExtends) || at(SyntaxKind::KwImplements)) {
        reportAtCurrent("An interface cannot extend or implement anything; its body lists "
                        "only method signatures");
        builder.startNode(SyntaxKind::Error);
        while (!at(SyntaxKind::LBrace) && !atEnd()) bump();
        builder.finishNode();
    }
    expect(SyntaxKind::LBrace, "'{'");
    builder.startNode(SyntaxKind::MemberList);
    while (!at(SyntaxKind::RBrace) && !atEnd()) {
        size_t before = current;
        parseStructOrClassMember();
        if (current == before) {
            reportAtCurrent("Unexpected token in interface body");
            recoverTo({SyntaxKind::RBrace, SyntaxKind::Semi, SyntaxKind::EndOfFile});
            eat(SyntaxKind::Semi);
            if (current == before && !atEnd()) bump();
        }
    }
    builder.finishNode();
    expect(SyntaxKind::RBrace, "'}'");
    builder.finishNode();
}

void Parser::parseStructOrClassMember() {
    if (looksLikeFuncDecl(/*allowShorthand=*/true) || looksLikeKeywordNamedMethod()) {
        parseFuncDecl();
    } else {
        parseFieldDecl();
    }
}

// A reserved word sitting where a member name is expected (`type() -> ...`):
// the shape of a method declaration whose name slot holds a keyword. Parsing
// it as a method keeps one bad name from cascading through the member list;
// parseFuncDecl reports the name.
bool Parser::looksLikeKeywordNamedMethod() const {
    size_t idx = current;
    auto skipTrivia = [&] {
        while (idx < tokens.size() && isTrivia(tokens[idx].kind)) idx++;
    };
    if (idx < tokens.size() && (tokens[idx].kind == SyntaxKind::KwPrivate ||
                                tokens[idx].kind == SyntaxKind::KwProtected ||
                                tokens[idx].kind == SyntaxKind::KwPublic)) {
        idx++;
        skipTrivia();
    }
    while (idx < tokens.size() && (tokens[idx].kind == SyntaxKind::KwOverride ||
                                   tokens[idx].kind == SyntaxKind::KwFinal ||
                                   tokens[idx].kind == SyntaxKind::KwAbstract)) {
        idx++;
        skipTrivia();
    }
    if (idx >= tokens.size() || !isKeyword(tokens[idx].kind)) return false;
    idx++;
    skipTrivia();
    return idx < tokens.size() && tokens[idx].kind == SyntaxKind::LParen;
}

void Parser::parseFieldDecl() {
    builder.startNode(SyntaxKind::FieldDecl);
    parseVisibilityModifier();
    if (at(SyntaxKind::KwWeak)) bump();
    if (isTypeStart(kindAt())) parseType();
    else emitMissing(SyntaxKind::Identifier, "field type");
    expect(SyntaxKind::Identifier, "field name");
    if (at(SyntaxKind::Eq)) parseDefaultValue();
    expect(SyntaxKind::Semi, "';' after field declaration");
    builder.finishNode();
}

void Parser::parseEnumDecl() {
    builder.startNode(SyntaxKind::EnumDecl);
    parseVisibilityModifier();
    expect(SyntaxKind::KwEnum, "'enum'");
    expect(SyntaxKind::Identifier, "enum name");
    expect(SyntaxKind::LBrace, "'{'");
    while (!at(SyntaxKind::RBrace) && !atEnd()) {
        size_t before = current;
        if (at(SyntaxKind::Identifier)) {
            builder.startNode(SyntaxKind::EnumMember);
            bump();
            builder.finishNode();
            eat(SyntaxKind::Comma);
        } else {
            reportAtCurrent("Expected an enum member name");
            recoverTo({SyntaxKind::Comma, SyntaxKind::RBrace, SyntaxKind::EndOfFile});
            eat(SyntaxKind::Comma);
        }
        if (current == before && !atEnd()) bump();
    }
    expect(SyntaxKind::RBrace, "'}'");
    builder.finishNode();
}

// =================================================================
// Types
// =================================================================

bool Parser::isPrimitiveTypeKw(SyntaxKind k) const {
    switch (k) {
        case SyntaxKind::KwBool:
        case SyntaxKind::KwByte:
        case SyntaxKind::KwShort:
        case SyntaxKind::KwUShort:
        case SyntaxKind::KwInt:
        case SyntaxKind::KwUInt:
        case SyntaxKind::KwLong:
        case SyntaxKind::KwULong:
        case SyntaxKind::KwFloat:
        case SyntaxKind::KwDouble:
        case SyntaxKind::KwDecimal:
        case SyntaxKind::KwChar:
        case SyntaxKind::KwString:
        case SyntaxKind::KwVoid:
            return true;
        default:
            return false;
    }
}

bool Parser::isTypeStart(SyntaxKind k) const {
    return k == SyntaxKind::Identifier || isPrimitiveTypeKw(k);
}

void Parser::parseType() {
    builder.startNode(SyntaxKind::TypeRef);
    bool wasIdentifier = at(SyntaxKind::Identifier);
    if (isTypeStart(kindAt())) bump();
    else emitMissing(SyntaxKind::Identifier, "type name");
    // Allow a single namespace qualifier: `ns.Name`. Primitives are not qualifiable.
    if (wasIdentifier && at(SyntaxKind::Dot) && peekKind(1) == SyntaxKind::Identifier) {
        bump();  // '.'
        bump();  // identifier
    }
    if (at(SyntaxKind::Lt) && scanTypeArgs(0) != 0) parseTypeArgList();
    // Interleaved suffix chain: any sequence of `?` and `[]` pairs.
    // Each '[' must be followed immediately by ']' to be a type-position
    // array suffix; otherwise it's left for the caller (e.g. `new T[n]`).
    while (true) {
        if (at(SyntaxKind::Question)) { bump(); continue; }
        if (at(SyntaxKind::LBracket) && peekKind(1) == SyntaxKind::RBracket) {
            bump();  // '['
            bump();  // ']'
            continue;
        }
        break;
    }
    builder.finishNode();
}

void Parser::parseTypeHead() {
    // Like parseType but leaves dim brackets for the NewExpr to parse.
    // A `[]` is consumed only when followed by `?` or another `[` (then it is
    // unambiguously part of an array element type like `T[]?` or `T[][n]`).
    // A standalone `[]` or `[size]` at this position belongs to the NewExpr's
    // dimension list.
    // `?` is always consumed so `new Box?[3]` constructs an array of
    // nullable Box rather than safe-subscripting the result of `new Box[3]`.
    builder.startNode(SyntaxKind::TypeRef);
    bool wasIdentifier = at(SyntaxKind::Identifier);
    if (isTypeStart(kindAt())) bump();
    else emitMissing(SyntaxKind::Identifier, "type name");
    if (wasIdentifier && at(SyntaxKind::Dot) && peekKind(1) == SyntaxKind::Identifier) {
        bump();
        bump();
    }
    if (at(SyntaxKind::Lt) && scanTypeArgs(0) != 0) parseTypeArgList();
    while (true) {
        if (at(SyntaxKind::Question)) { bump(); continue; }
        if (at(SyntaxKind::LBracket) &&
            peekKind(1) == SyntaxKind::RBracket &&
            (peekKind(2) == SyntaxKind::Question ||
             peekKind(2) == SyntaxKind::LBracket)) {
            bump();  // '['
            bump();  // ']'
            continue;
        }
        break;
    }
    builder.finishNode();
}

void Parser::parseTypeParamList() {
    builder.startNode(SyntaxKind::TypeParamList);
    expect(SyntaxKind::Lt, "'<'");
    if (!atClosingGt() && !atEnd()) {
        parseTypeParam();
        while (eat(SyntaxKind::Comma)) {
            if (atClosingGt()) break;
            parseTypeParam();
        }
    }
    expectClosingGt("'>' after type parameters");
    builder.finishNode();
}

void Parser::parseTypeParam() {
    builder.startNode(SyntaxKind::TypeParam);
    expect(SyntaxKind::Identifier, "type parameter name");
    if (eat(SyntaxKind::Colon)) {
        // One or more bounds separated by '+': `T: Base + Comparable`.
        if (isTypeStart(kindAt())) parseType();
        else emitMissing(SyntaxKind::Identifier, "bound type after ':'");
        while (eat(SyntaxKind::Plus)) {
            if (isTypeStart(kindAt())) parseType();
            else emitMissing(SyntaxKind::Identifier, "bound type after '+'");
        }
    }
    builder.finishNode();
}

void Parser::parseTypeArgList() {
    builder.startNode(SyntaxKind::TypeArgList);
    expect(SyntaxKind::Lt, "'<'");
    if (!atClosingGt() && !atEnd()) {
        parseType();
        while (eat(SyntaxKind::Comma)) {
            if (atClosingGt()) break;
            parseType();
        }
    }
    expectClosingGt("'>' after type arguments");
    builder.finishNode();
}

bool Parser::atClosingGt() const {
    SyntaxKind k = kindAt();
    return k == SyntaxKind::Gt || k == SyntaxKind::GtGt || k == SyntaxKind::GtGtGt;
}

void Parser::expectClosingGt(const char* what) {
    SyntaxKind k = kindAt();
    if (k == SyntaxKind::Gt) { bump(); return; }
    if (k == SyntaxKind::GtGt || k == SyntaxKind::GtGtGt) {
        // A merged '>>'/'>>>' closes nested generics: emit one '>', keep the rest.
        while (nextToEmit < tokens.size() && nextToEmit < current) {
            const auto& t = tokens[nextToEmit];
            builder.token(t.kind, t.text);
            nextToEmit++;
        }
        builder.token(SyntaxKind::Gt, u">");
        LexedToken& tok = tokens[current];
        tok.kind = (k == SyntaxKind::GtGt) ? SyntaxKind::Gt : SyntaxKind::GtGt;
        tok.text = tok.text.substr(1);
        tok.offset += 1;
        tok.column += 1;
        nextToEmit = current;
        return;
    }
    emitMissing(SyntaxKind::Gt, what);
}

size_t Parser::scanTypeArgs(size_t cursor) const {
    if (peekKind(cursor) != SyntaxKind::Lt) return 0;
    int depth = 0;
    size_t c = cursor;
    for (int guard = 0; guard < 256; ++guard) {
        SyntaxKind k = peekKind(c);
        switch (k) {
            case SyntaxKind::Lt:     depth += 1; break;
            case SyntaxKind::LtLt:   depth += 2; break;
            case SyntaxKind::Gt:     depth -= 1; break;
            case SyntaxKind::GtGt:   depth -= 2; break;
            case SyntaxKind::GtGtGt: depth -= 3; break;
            case SyntaxKind::Identifier:
            case SyntaxKind::Comma:
            case SyntaxKind::Dot:
            case SyntaxKind::Question:
            case SyntaxKind::LBracket:
            case SyntaxKind::RBracket:
                break;
            default:
                if (!isPrimitiveTypeKw(k)) return 0;
        }
        c++;
        if (depth <= 0) return c;
    }
    return 0;
}

size_t Parser::skipAnglesRaw(size_t idx) const {
    if (idx >= tokens.size() || tokens[idx].kind != SyntaxKind::Lt) return idx;
    int depth = 0;
    size_t i = idx;
    for (int guard = 0; guard < 256 && i < tokens.size(); ++guard) {
        SyntaxKind k = tokens[i].kind;
        if (k == SyntaxKind::Lt)          depth += 1;
        else if (k == SyntaxKind::LtLt)   depth += 2;
        else if (k == SyntaxKind::Gt)     depth -= 1;
        else if (k == SyntaxKind::GtGt)   depth -= 2;
        else if (k == SyntaxKind::GtGtGt) depth -= 3;
        else if (k == SyntaxKind::EndOfFile || k == SyntaxKind::LBrace ||
                 k == SyntaxKind::Semi || k == SyntaxKind::LParen ||
                 k == SyntaxKind::RParen) return idx;
        i++;
        if (depth <= 0) return i;
    }
    return idx;
}


bool Parser::looksLikeTypedVarDecl() const {
    return looksLikeTypedVarDeclFrom(0);
}

bool Parser::looksLikeTypedVarDeclFrom(size_t start) const {
    SyntaxKind k0 = peekKind(start);
    if (!isTypeStart(k0)) return false;
    // Skip a single optional namespace qualifier: `ns.Name ...`. Primitives can't
    // be qualified, so this only fires when the leading token is an Identifier.
    size_t cursor = start + 1;
    if (k0 == SyntaxKind::Identifier &&
        peekKind(start + 1) == SyntaxKind::Dot &&
        peekKind(start + 2) == SyntaxKind::Identifier) {
        cursor = start + 3;
    }
    if (size_t a = scanTypeArgs(cursor)) cursor = a;
    // Skip any interleaved sequence of `?` and `[]` type suffixes.
    while (true) {
        if (peekKind(cursor) == SyntaxKind::Question) { cursor += 1; continue; }
        if (peekKind(cursor) == SyntaxKind::LBracket &&
            peekKind(cursor + 1) == SyntaxKind::RBracket) {
            cursor += 2;
            continue;
        }
        break;
    }
    SyntaxKind name = peekKind(cursor);
    SyntaxKind afterName = peekKind(cursor + 1);
    if (name != SyntaxKind::Identifier) return false;
    return afterName == SyntaxKind::Eq || afterName == SyntaxKind::Semi;
}

// =================================================================
// Statements
// =================================================================

void Parser::parseStatement() {
    SyntaxKind k = kindAt();
    if (k == SyntaxKind::LBrace)        { parseBlock(); return; }
    if (k == SyntaxKind::KwLet)         { parseLetStmt(); return; }
    if (k == SyntaxKind::KwConst)       { parseConstDecl(); return; }
    if (k == SyntaxKind::KwIf)          { parseIfStmt(); return; }
    if (k == SyntaxKind::KwWhile)       { parseWhileStmt(); return; }
    if (k == SyntaxKind::KwFor)         { parseForStmt(); return; }
    if (k == SyntaxKind::KwSwitch)      { parseSwitchStmt(); return; }
    if (k == SyntaxKind::KwBreak)       { parseBreakStmt(); return; }
    if (k == SyntaxKind::KwContinue)    { parseContinueStmt(); return; }
    if (k == SyntaxKind::KwReturn)      { parseReturnStmt(); return; }
    if (k == SyntaxKind::KwThrow)       { parseThrowStmt(); return; }
    if (k == SyntaxKind::KwRethrow)     { parseRethrowStmt(); return; }
    if (k == SyntaxKind::KwCatch) {
        reportAtCurrent("'catch' can only appear after a function body's closing '}'");
        recoverTo({SyntaxKind::Semi, SyntaxKind::RBrace, SyntaxKind::EndOfFile});
        eat(SyntaxKind::Semi);
        return;
    }
    if (k == SyntaxKind::KwFinally) {
        reportAtCurrent("'finally' is not supported");
        recoverTo({SyntaxKind::Semi, SyntaxKind::RBrace, SyntaxKind::EndOfFile});
        eat(SyntaxKind::Semi);
        return;
    }
    if (looksLikeTypedVarDecl())        { parseTypedVarDeclStmt(); return; }
    parseExprStmt();
}

void Parser::parseBlock() {
    builder.startNode(SyntaxKind::Block);
    expect(SyntaxKind::LBrace, "'{'");
    while (!at(SyntaxKind::RBrace) && !atEnd()) {
        size_t before = current;
        parseStatement();
        if (current == before) {
            reportAtCurrent("Unexpected token in block");
            recoverTo({SyntaxKind::Semi, SyntaxKind::RBrace, SyntaxKind::EndOfFile});
            eat(SyntaxKind::Semi);
            if (current == before && !atEnd()) bump();
        }
    }
    expect(SyntaxKind::RBrace, "'}'");
    builder.finishNode();
}

void Parser::parseLetStmt() {
    builder.startNode(SyntaxKind::LetStmt);
    expect(SyntaxKind::KwLet, "'let'");
    expect(SyntaxKind::Identifier, "identifier after 'let'");
    if (eat(SyntaxKind::Eq)) parseExpression();
    expect(SyntaxKind::Semi, "';' after let declaration");
    builder.finishNode();
}

void Parser::parseTypedVarDeclStmt() {
    builder.startNode(SyntaxKind::TypedVarDecl);
    parseType();
    expect(SyntaxKind::Identifier, "identifier after type");
    if (eat(SyntaxKind::Eq)) parseExpression();
    expect(SyntaxKind::Semi, "';' after declaration");
    builder.finishNode();
}

void Parser::parseIfStmt() {
    builder.startNode(SyntaxKind::IfStmt);
    expect(SyntaxKind::KwIf, "'if'");
    expect(SyntaxKind::LParen, "'(' around if condition");
    parseExpression();
    expect(SyntaxKind::RParen, "')' after if condition");
    if (at(SyntaxKind::LBrace)) parseBlock();
    else { emitMissing(SyntaxKind::LBrace, "'{' for if body"); }
    if (at(SyntaxKind::KwElse)) {
        builder.startNode(SyntaxKind::ElseClause);
        bump();
        if (at(SyntaxKind::KwIf)) parseIfStmt();
        else if (at(SyntaxKind::LBrace)) parseBlock();
        else emitMissing(SyntaxKind::LBrace, "'{' or 'if' after 'else'");
        builder.finishNode();
    }
    builder.finishNode();
}

void Parser::parseWhileStmt() {
    builder.startNode(SyntaxKind::WhileStmt);
    expect(SyntaxKind::KwWhile, "'while'");
    expect(SyntaxKind::LParen, "'(' around while condition");
    parseExpression();
    expect(SyntaxKind::RParen, "')' after while condition");
    if (at(SyntaxKind::LBrace)) parseBlock();
    else emitMissing(SyntaxKind::LBrace, "'{' for while body");
    builder.finishNode();
}

void Parser::parseConstDecl() {
    // `const T x = expr;` reuses the typed-var-decl shape; `const x = expr;` reuses
    // the let shape. Both keep the leading `const` token as a child.
    if (looksLikeTypedVarDeclFrom(1)) {
        builder.startNode(SyntaxKind::TypedVarDecl);
        expect(SyntaxKind::KwConst, "'const'");
        parseType();
        expect(SyntaxKind::Identifier, "identifier after type");
        if (eat(SyntaxKind::Eq)) parseExpression();
        expect(SyntaxKind::Semi, "';' after declaration");
        builder.finishNode();
    } else {
        builder.startNode(SyntaxKind::LetStmt);
        expect(SyntaxKind::KwConst, "'const'");
        expect(SyntaxKind::Identifier, "identifier after 'const'");
        if (eat(SyntaxKind::Eq)) parseExpression();
        expect(SyntaxKind::Semi, "';' after const declaration");
        builder.finishNode();
    }
}

bool Parser::looksLikeForeachHeader() const {
    // Positioned at the first token inside the for parentheses.
    SyntaxKind k0 = peekKind(0);
    if (k0 == SyntaxKind::KwLet || k0 == SyntaxKind::KwConst) {
        return peekKind(1) == SyntaxKind::Identifier && peekIsContextualIn(2);
    }
    if (!isTypeStart(k0)) return false;
    size_t cursor = 1;
    if (k0 == SyntaxKind::Identifier &&
        peekKind(1) == SyntaxKind::Dot &&
        peekKind(2) == SyntaxKind::Identifier) {
        cursor = 3;
    }
    while (true) {
        if (peekKind(cursor) == SyntaxKind::Question) { cursor += 1; continue; }
        if (peekKind(cursor) == SyntaxKind::LBracket &&
            peekKind(cursor + 1) == SyntaxKind::RBracket) {
            cursor += 2;
            continue;
        }
        break;
    }
    if (peekKind(cursor) != SyntaxKind::Identifier) return false;
    return peekIsContextualIn(cursor + 1);
}

void Parser::parseForStmt() {
    // The node kind (foreach vs C-style) is known only after `for (`, so open it
    // retroactively from a checkpoint.
    size_t cp = builder.checkpoint();
    expect(SyntaxKind::KwFor, "'for'");
    expect(SyntaxKind::LParen, "'(' after 'for'");

    if (looksLikeForeachHeader()) {
        builder.startNodeAt(cp, SyntaxKind::ForEachStmt);
        if (at(SyntaxKind::KwLet) || at(SyntaxKind::KwConst)) bump();
        else parseType();
        expect(SyntaxKind::Identifier, "loop variable name");
        if (atContextualIn()) bumpAs(SyntaxKind::KwIn);
        else emitMissing(SyntaxKind::KwIn, "'in' in for-each loop");
        parseExpression();
        expect(SyntaxKind::RParen, "')' after for-each header");
        if (at(SyntaxKind::LBrace)) parseBlock();
        else emitMissing(SyntaxKind::LBrace, "'{' for loop body");
        builder.finishNode();
        return;
    }

    builder.startNodeAt(cp, SyntaxKind::ForStmt);
    // init clause (each statement parser consumes its own ';')
    if (at(SyntaxKind::Semi)) bump();
    else if (at(SyntaxKind::KwLet)) parseLetStmt();
    else if (at(SyntaxKind::KwConst)) parseConstDecl();
    else if (looksLikeTypedVarDecl()) parseTypedVarDeclStmt();
    else parseExprStmt();
    // condition clause
    if (!at(SyntaxKind::Semi)) parseExpression();
    expect(SyntaxKind::Semi, "';' after for condition");
    // update clause
    if (!at(SyntaxKind::RParen)) {
        builder.startNode(SyntaxKind::ForUpdate);
        parseExpression();
        builder.finishNode();
    }
    expect(SyntaxKind::RParen, "')' after for clauses");
    if (at(SyntaxKind::LBrace)) parseBlock();
    else emitMissing(SyntaxKind::LBrace, "'{' for loop body");
    builder.finishNode();
}

void Parser::parseBreakStmt() {
    builder.startNode(SyntaxKind::BreakStmt);
    expect(SyntaxKind::KwBreak, "'break'");
    expect(SyntaxKind::Semi, "';' after break");
    builder.finishNode();
}

void Parser::parseContinueStmt() {
    builder.startNode(SyntaxKind::ContinueStmt);
    expect(SyntaxKind::KwContinue, "'continue'");
    expect(SyntaxKind::Semi, "';' after continue");
    builder.finishNode();
}

void Parser::parseReturnStmt() {
    builder.startNode(SyntaxKind::ReturnStmt);
    expect(SyntaxKind::KwReturn, "'return'");
    if (!at(SyntaxKind::Semi) && !atEnd()) parseExpression();
    expect(SyntaxKind::Semi, "';' after return");
    builder.finishNode();
}

void Parser::parseThrowStmt() {
    builder.startNode(SyntaxKind::ThrowStmt);
    expect(SyntaxKind::KwThrow, "'throw'");
    if (at(SyntaxKind::Semi)) {
        reportAtCurrent("'throw' needs an exception value, e.g. 'throw new Error(\"...\");'. "
                        "To rethrow the current exception inside a catch block, use 'rethrow;'");
    } else if (!atEnd()) {
        parseExpression();
    }
    expect(SyntaxKind::Semi, "';' after throw");
    builder.finishNode();
}

void Parser::parseRethrowStmt() {
    builder.startNode(SyntaxKind::RethrowStmt);
    expect(SyntaxKind::KwRethrow, "'rethrow'");
    expect(SyntaxKind::Semi, "';' after rethrow");
    builder.finishNode();
}

void Parser::parseExprStmt() {
    builder.startNode(SyntaxKind::ExprStmt);
    parseExpression();
    expect(SyntaxKind::Semi, "';' after expression");
    builder.finishNode();
}

void Parser::parseSwitchStmt() {
    builder.startNode(SyntaxKind::SwitchStmt);
    expect(SyntaxKind::KwSwitch, "'switch'");
    parseSwitchHeaderAndArms();
    builder.finishNode();
}

void Parser::parseSwitchExpr() {
    builder.startNode(SyntaxKind::SwitchExpr);
    expect(SyntaxKind::KwSwitch, "'switch'");
    parseSwitchHeaderAndArms();
    builder.finishNode();
}

void Parser::parseSwitchHeaderAndArms() {
    expect(SyntaxKind::LParen, "'(' after 'switch'");
    parseExpression();
    expect(SyntaxKind::RParen, "')' after the switch value");
    expect(SyntaxKind::LBrace, "'{' to begin the switch body");
    while (!at(SyntaxKind::RBrace) && !atEnd()) {
        size_t before = current;
        parseSwitchArm();
        if (current == before) {
            reportAtCurrent("Unexpected token in switch body");
            recoverTo({SyntaxKind::Comma, SyntaxKind::RBrace, SyntaxKind::EndOfFile});
            eat(SyntaxKind::Comma);
            if (current == before && !atEnd()) bump();
        }
    }
    expect(SyntaxKind::RBrace, "'}' to close the switch body");
}

void Parser::parseSwitchArm() {
    builder.startNode(SyntaxKind::SwitchArm);
    if (at(SyntaxKind::KwDefault)) {
        bump();
    } else if (at(SyntaxKind::KwIs)) {
        // Type arm: `is Type [binding] -> body`. A bare `is Type` is not an
        // expression, so this form gets its own path.
        bump();  // 'is'
        if (isTypeStart(kindAt())) parseType();
        else emitMissing(SyntaxKind::Identifier, "type after 'is'");
        eat(SyntaxKind::Identifier);  // optional binding name
    } else {
        // One or more comma-separated labels, ending at the `->`.
        parseExpression();
        while (eat(SyntaxKind::Comma)) {
            if (at(SyntaxKind::Arrow) || at(SyntaxKind::RBrace) || atEnd()) break;
            parseExpression();
        }
    }
    expect(SyntaxKind::Arrow, "'->' after the switch label");
    if (at(SyntaxKind::LBrace)) parseBlock();
    else parseExpression();
    eat(SyntaxKind::Comma);
    builder.finishNode();
}

// =================================================================
// Expressions (Pratt)
// =================================================================

bool Parser::isAssignmentOp(SyntaxKind k) const {
    switch (k) {
        case SyntaxKind::Eq:
        case SyntaxKind::PlusEq:
        case SyntaxKind::MinusEq:
        case SyntaxKind::StarEq:
        case SyntaxKind::SlashEq:
        case SyntaxKind::PercentEq:
        case SyntaxKind::AmpEq:
        case SyntaxKind::PipeEq:
        case SyntaxKind::CaretEq:
        case SyntaxKind::LtLtEq:
        case SyntaxKind::GtGtEq:
        case SyntaxKind::GtGtGtEq:
            return true;
        default:
            return false;
    }
}

int Parser::infixPrecedence(SyntaxKind k) const {
    if (isAssignmentOp(k)) return 1;
    switch (k) {
        case SyntaxKind::Question:   return 2;   // ternary
        case SyntaxKind::PipePipe:   return 3;
        case SyntaxKind::AmpAmp:     return 4;
        case SyntaxKind::Pipe:       return 5;
        case SyntaxKind::Caret:      return 6;
        case SyntaxKind::Amp:        return 7;
        case SyntaxKind::EqEq:
        case SyntaxKind::NotEq:      return 8;
        case SyntaxKind::Lt:
        case SyntaxKind::Gt:
        case SyntaxKind::LtEq:
        case SyntaxKind::GtEq:
        case SyntaxKind::KwIs:       return 9;  // type test - comparison level
        case SyntaxKind::LtLt:
        case SyntaxKind::GtGt:
        case SyntaxKind::GtGtGt:     return 10;
        case SyntaxKind::Plus:
        case SyntaxKind::Minus:      return 11;
        case SyntaxKind::Star:
        case SyntaxKind::Slash:
        case SyntaxKind::Percent:    return 12;
        case SyntaxKind::KwAs:       return 13;  // cast - binds tighter than * and unary
        case SyntaxKind::Dot:
        case SyntaxKind::QuestionDot:
        case SyntaxKind::LParen:
        case SyntaxKind::LBracket:
        case SyntaxKind::PlusPlus:
        case SyntaxKind::MinusMinus: return 14;  // postfix
        default:                     return 0;
    }
}

void Parser::parseExpression() {
    parsePrecedence(1);
}

void Parser::parsePrecedence(int minPrec) {
    size_t cp = builder.checkpoint();
    parsePrefix();

    while (true) {
        SyntaxKind op = kindAt();
        // Generic call `callee<TypeArgs>(args)`: a balanced type-ish `<...>`
        // immediately followed by `(`. Otherwise `<` is a comparison operator.
        if (op == SyntaxKind::Lt) {
            size_t after = scanTypeArgs(0);
            if (after != 0 && peekKind(after) == SyntaxKind::LParen) {
                builder.startNodeAt(cp, SyntaxKind::CallExpr);
                parseTypeArgList();
                parseArgList();
                builder.finishNode();
                continue;
            }
        }
        // `?[` is a postfix safe-subscript when written adjacent; otherwise `?`
        // is the loose-binding ternary operator. We disambiguate by lookahead
        // so the lexer doesn't have to merge `?[`, which would otherwise break
        // type syntax like `Box?[]?[]`.
        bool isSafeSubscript = (op == SyntaxKind::Question &&
                                peekKind(1) == SyntaxKind::LBracket);
        // `??` is two adjacent `?` tokens; like `?[` it is disambiguated here rather
        // than merged in the lexer, so type syntax such as `T??` keeps working.
        bool isNullCoalesce = (op == SyntaxKind::Question &&
                               peekKind(1) == SyntaxKind::Question);
        int prec = isSafeSubscript ? 14 : (isNullCoalesce ? 3 : infixPrecedence(op));
        if (prec < minPrec) break;

        if (isSafeSubscript) {
            builder.startNodeAt(cp, SyntaxKind::SafeSubscriptExpr);
            bump();  // '?'
            bump();  // '['
            parseExpression();
            expect(SyntaxKind::RBracket, "']' after safe subscript");
            builder.finishNode();
            continue;
        }

        if (isNullCoalesce) {
            builder.startNodeAt(cp, SyntaxKind::NullCoalesceExpr);
            bump();  // '?'
            bump();  // '?'
            parsePrecedence(prec);  // right associative
            builder.finishNode();
            continue;
        }

        if (op == SyntaxKind::LParen) {
            builder.startNodeAt(cp, SyntaxKind::CallExpr);
            parseArgList();
            builder.finishNode();
            continue;
        }
        if (op == SyntaxKind::LBracket) {
            builder.startNodeAt(cp, SyntaxKind::SubscriptExpr);
            bump();
            parseExpression();
            expect(SyntaxKind::RBracket, "']' after subscript");
            builder.finishNode();
            continue;
        }
        if (op == SyntaxKind::PlusPlus || op == SyntaxKind::MinusMinus) {
            // Postfix `++`/`--` wraps the operand parsed so far; it takes no
            // right operand and binds as tightly as the other postfix forms.
            builder.startNodeAt(cp, SyntaxKind::PostfixExpr);
            bump();
            builder.finishNode();
            continue;
        }
        if (op == SyntaxKind::Dot) {
            builder.startNodeAt(cp, SyntaxKind::MemberExpr);
            bump();
            expect(SyntaxKind::Identifier, "identifier after '.'");
            builder.finishNode();
            continue;
        }
        if (op == SyntaxKind::QuestionDot) {
            builder.startNodeAt(cp, SyntaxKind::SafeMemberExpr);
            bump();
            expect(SyntaxKind::Identifier, "identifier after '?.'");
            builder.finishNode();
            continue;
        }
        if (op == SyntaxKind::KwAs) {
            // A '?' directly after 'as' selects the checked cast `as? Type`.
            bool checked = peekKind(1) == SyntaxKind::Question;
            builder.startNodeAt(cp, checked ? SyntaxKind::CheckedCastExpr : SyntaxKind::CastExpr);
            bump();  // 'as'
            if (checked) bump();  // '?'
            if (isTypeStart(kindAt())) parseType();
            else emitMissing(SyntaxKind::Identifier, checked ? "type after 'as?'" : "type after 'as'");
            builder.finishNode();
            continue;
        }
        if (op == SyntaxKind::KwIs) {
            builder.startNodeAt(cp, SyntaxKind::TypeTestExpr);
            bump();  // 'is'
            if (isTypeStart(kindAt())) parseType();
            else emitMissing(SyntaxKind::Identifier, "type after 'is'");
            builder.finishNode();
            continue;
        }
        if (op == SyntaxKind::Question) {
            builder.startNodeAt(cp, SyntaxKind::TernaryExpr);
            bump();
            parseExpression();
            expect(SyntaxKind::Colon, "':' in ternary expression");
            parsePrecedence(1);
            builder.finishNode();
            continue;
        }
        if (isAssignmentOp(op)) {
            builder.startNodeAt(cp, SyntaxKind::AssignExpr);
            bump();
            parsePrecedence(prec);
            builder.finishNode();
            continue;
        }
        builder.startNodeAt(cp, SyntaxKind::BinaryExpr);
        bump();
        parsePrecedence(prec + 1);
        builder.finishNode();
    }
}

void Parser::parsePrefix() {
    SyntaxKind k = kindAt();
    switch (k) {
        case SyntaxKind::KwTry: {
            // `try` prefixes exactly one call: one primary plus its postfix chain
            // (`.`, `?.`, `(...)`, `[...]`), nothing looser. The analyzer requires
            // the operand to be a call expression.
            builder.startNode(SyntaxKind::TryExpr);
            bump();
            parsePrecedence(14);
            builder.finishNode();
            return;
        }
        case SyntaxKind::KwSwitch:
            parseSwitchExpr();
            return;
        case SyntaxKind::IntLiteral:
        case SyntaxKind::LongLiteral:
        case SyntaxKind::FloatLiteral:
        case SyntaxKind::DoubleLiteral:
        case SyntaxKind::StringLiteral:
        case SyntaxKind::CharLiteral:
        case SyntaxKind::KwTrue:
        case SyntaxKind::KwFalse:
        case SyntaxKind::KwNull:
            builder.startNode(SyntaxKind::LiteralExpr);
            bump();
            builder.finishNode();
            return;
        case SyntaxKind::InterpStringStart: {
            // "text {expr} text": alternating literal segments and hole
            // expressions. Each hole is followed by a Mid or End segment.
            builder.startNode(SyntaxKind::InterpStringExpr);
            bump();  // leading "...{ segment
            while (true) {
                parseExpression();
                if (eat(SyntaxKind::InterpStringMid)) continue;
                if (at(SyntaxKind::InterpStringEnd)) { bump(); break; }
                expect(SyntaxKind::InterpStringEnd, "end of interpolated string");
                break;
            }
            builder.finishNode();
            return;
        }
        case SyntaxKind::Identifier:
            builder.startNode(SyntaxKind::IdentExpr);
            bump();
            builder.finishNode();
            return;
        case SyntaxKind::KwString:
            // The `string` type name as an expression receiver, for static
            // builtins like `string.fromBytes(bytes)`.
            builder.startNode(SyntaxKind::IdentExpr);
            bump();
            builder.finishNode();
            return;
        case SyntaxKind::KwThis:
            builder.startNode(SyntaxKind::ThisExpr);
            bump();
            builder.finishNode();
            return;
        case SyntaxKind::KwSuper:
            builder.startNode(SyntaxKind::SuperExpr);
            bump();
            builder.finishNode();
            return;
        case SyntaxKind::KwNew: {
            builder.startNode(SyntaxKind::NewExpr);
            bump();
            // The element type must NOT eat trailing `[]` here, those belong
            // to the NewExpr's dimension list (`new T[a][]`).
            if (isTypeStart(kindAt())) parseTypeHead();
            else emitMissing(SyntaxKind::Identifier, "type name after 'new'");
            if (at(SyntaxKind::LBracket)) {
                // Accept one or more `[expr]` brackets for multi-dim allocation,
                // optionally followed by `[]` brackets for partially-allocated
                // tails (`new T[a][]` leaves the inner slots null).
                bool seenEmpty = false;
                while (at(SyntaxKind::LBracket)) {
                    bump();  // '['
                    if (at(SyntaxKind::RBracket)) {
                        bump();  // ']' (empty bracket)
                        seenEmpty = true;
                        continue;
                    }
                    if (seenEmpty) {
                        reportAtCurrent(
                            "Sized dimensions must come before any '[]' dimensions");
                    }
                    parseExpression();
                    expect(SyntaxKind::RBracket, "']' after array size");
                }
            } else if (at(SyntaxKind::LParen)) {
                parseArgList();
            } else {
                emitMissing(SyntaxKind::LParen, "'(' or '[' after type in 'new'");
            }
            builder.finishNode();
            return;
        }
        case SyntaxKind::LParen: {
            builder.startNode(SyntaxKind::ParenExpr);
            bump();
            parseExpression();
            expect(SyntaxKind::RParen, "')'");
            builder.finishNode();
            return;
        }
        case SyntaxKind::Minus:
        case SyntaxKind::Bang:
        case SyntaxKind::PlusPlus:
        case SyntaxKind::MinusMinus: {
            builder.startNode(SyntaxKind::PrefixExpr);
            bump();
            parsePrecedence(13);
            builder.finishNode();
            return;
        }
        case SyntaxKind::LBracket: {
            // Array literal: `[ ]`, `[ expr (',' expr)* ','? ]`.
            // Subscript `arr[i]` does not enter parsePrefix - it is handled as
            // a postfix in parsePrecedence after a left-hand expression exists.
            builder.startNode(SyntaxKind::ArrayLiteralExpr);
            bump();  // '['
            if (!at(SyntaxKind::RBracket) && !atEnd()) {
                parseExpression();
                while (eat(SyntaxKind::Comma)) {
                    if (at(SyntaxKind::RBracket)) break;  // trailing comma OK
                    parseExpression();
                }
            }
            expect(SyntaxKind::RBracket, "']' to close array literal");
            builder.finishNode();
            return;
        }
        default:
            emitMissing(SyntaxKind::Identifier, "expression");
            return;
    }
}

void Parser::parseArgList() {
    builder.startNode(SyntaxKind::ArgList);
    expect(SyntaxKind::LParen, "'('");
    if (!at(SyntaxKind::RParen) && !atEnd()) {
        parseCallArgument();
        while (eat(SyntaxKind::Comma)) {
            if (at(SyntaxKind::RParen)) break;
            parseCallArgument();
        }
    }
    expect(SyntaxKind::RParen, "')'");
    builder.finishNode();
}

void Parser::parseCallArgument() {
    // `out name`: only when `out` is directly followed by the target identifier.
    if (atContextualOut() && peekKind(1) == SyntaxKind::Identifier) {
        builder.startNode(SyntaxKind::OutArgument);
        bumpAs(SyntaxKind::KwOut);  // 'out'
        bump();                     // target identifier
        builder.finishNode();
        return;
    }
    // `name: expr`: a named argument. Unambiguous at argument start because no
    // expression begins with an identifier directly followed by ':'.
    if (at(SyntaxKind::Identifier) && peekKind(1) == SyntaxKind::Colon) {
        builder.startNode(SyntaxKind::NamedArgument);
        bump();  // parameter name
        bump();  // ':'
        parseExpression();
        builder.finishNode();
        return;
    }
    parseExpression();
}
