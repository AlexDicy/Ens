#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include "SyntaxKind.h"

class GreenElement {
public:
    SyntaxKind kind;
    uint32_t length;

    GreenElement(SyntaxKind k, uint32_t len) : kind(k), length(len) {}
    virtual ~GreenElement() = default;

    bool isToken() const { return ::isToken(kind) || kind == SyntaxKind::Missing; }
};

using GreenElementPtr = std::unique_ptr<GreenElement>;

class GreenToken final : public GreenElement {
public:
    std::u16string text;

    GreenToken(SyntaxKind k, std::u16string t)
        : GreenElement(k, static_cast<uint32_t>(t.size())), text(std::move(t)) {}
};

class GreenNode final : public GreenElement {
public:
    std::vector<GreenElementPtr> children;

    GreenNode(SyntaxKind k, std::vector<GreenElementPtr> ch);
};
