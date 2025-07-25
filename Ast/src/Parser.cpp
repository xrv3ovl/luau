// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
#include "Luau/Parser.h"

#include "Luau/Common.h"
#include "Luau/TimeTrace.h"

#include <algorithm>

#include <errno.h>
#include <limits.h>
#include <string.h>

LUAU_FASTINTVARIABLE(LuauRecursionLimit, 1000)
LUAU_FASTINTVARIABLE(LuauTypeLengthLimit, 1000)
LUAU_FASTINTVARIABLE(LuauParseErrorLimit, 100)

// Warning: If you are introducing new syntax, ensure that it is behind a separate
// flag so that we don't break production games by reverting syntax changes.
// See docs/SyntaxChanges.md for an explanation.
LUAU_FASTFLAGVARIABLE(LuauSolverV2)
LUAU_FASTFLAGVARIABLE(LuauDeclareExternType)
LUAU_FASTFLAGVARIABLE(LuauParseStringIndexer)
LUAU_FASTFLAGVARIABLE(LuauParseAttributeFixUninit)
LUAU_DYNAMIC_FASTFLAGVARIABLE(DebugLuauReportReturnTypeVariadicWithTypeSuffix, false)

// Clip with DebugLuauReportReturnTypeVariadicWithTypeSuffix
bool luau_telemetry_parsed_return_type_variadic_with_type_suffix = false;

namespace Luau
{

struct AttributeEntry
{
    const char* name;
    AstAttr::Type type;
};

AttributeEntry kAttributeEntries[] = {
    {"@checked", AstAttr::Type::Checked},
    {"@native", AstAttr::Type::Native},
    {"@deprecated", AstAttr::Type::Deprecated},
    {nullptr, AstAttr::Type::Checked}
};

ParseError::ParseError(const Location& location, std::string message)
    : location(location)
    , message(std::move(message))
{
}

const char* ParseError::what() const throw()
{
    return message.c_str();
}

const Location& ParseError::getLocation() const
{
    return location;
}

const std::string& ParseError::getMessage() const
{
    return message;
}

// LUAU_NOINLINE is used to limit the stack cost of this function due to std::string object / exception plumbing
LUAU_NOINLINE void ParseError::raise(const Location& location, const char* format, ...)
{
    va_list args;
    va_start(args, format);
    std::string message = vformat(format, args);
    va_end(args);

    throw ParseError(location, message);
}

ParseErrors::ParseErrors(std::vector<ParseError> errors)
    : errors(std::move(errors))
{
    LUAU_ASSERT(!this->errors.empty());

    if (this->errors.size() == 1)
        message = this->errors.front().what();
    else
        message = format("%d parse errors", int(this->errors.size()));
}

const char* ParseErrors::what() const throw()
{
    return message.c_str();
}

const std::vector<ParseError>& ParseErrors::getErrors() const
{
    return errors;
}

template<typename T>
TempVector<T>::TempVector(std::vector<T>& storage)
    : storage(storage)
    , offset(storage.size())
    , size_(0)
{
}

template<typename T>
TempVector<T>::~TempVector()
{
    LUAU_ASSERT(storage.size() == offset + size_);
    storage.erase(storage.begin() + offset, storage.end());
}

template<typename T>
const T& TempVector<T>::operator[](size_t index) const
{
    LUAU_ASSERT(index < size_);
    return storage[offset + index];
}

template<typename T>
const T& TempVector<T>::front() const
{
    LUAU_ASSERT(size_ > 0);
    return storage[offset];
}

template<typename T>
const T& TempVector<T>::back() const
{
    LUAU_ASSERT(size_ > 0);
    return storage.back();
}

template<typename T>
bool TempVector<T>::empty() const
{
    return size_ == 0;
}

template<typename T>
size_t TempVector<T>::size() const
{
    return size_;
}

template<typename T>
void TempVector<T>::push_back(const T& item)
{
    LUAU_ASSERT(storage.size() == offset + size_);
    storage.push_back(item);
    size_++;
}

static bool shouldParseTypePack(Lexer& lexer)
{
    if (lexer.current().type == Lexeme::Dot3)
        return true;
    else if (lexer.current().type == Lexeme::Name && lexer.lookahead().type == Lexeme::Dot3)
        return true;

    return false;
}

ParseResult Parser::parse(const char* buffer, size_t bufferSize, AstNameTable& names, Allocator& allocator, ParseOptions options)
{
    LUAU_TIMETRACE_SCOPE("Parser::parse", "Parser");

    Parser p(buffer, bufferSize, names, allocator, options);

    try
    {
        AstStatBlock* root = p.parseChunk();
        size_t lines = p.lexer.current().location.end.line + (bufferSize > 0 && buffer[bufferSize - 1] != '\n');

        return ParseResult{root, lines, std::move(p.hotcomments), std::move(p.parseErrors), std::move(p.commentLocations), std::move(p.cstNodeMap)};
    }
    catch (ParseError& err)
    {
        // when catching a fatal error, append it to the list of non-fatal errors and return
        p.parseErrors.push_back(err);

        return ParseResult{nullptr, 0, {}, p.parseErrors, {}, std::move(p.cstNodeMap)};
    }
}

ParseExprResult Parser::parseExpr(const char* buffer, size_t bufferSize, AstNameTable& names, Allocator& allocator, ParseOptions options)
{
    LUAU_TIMETRACE_SCOPE("Parser::parse", "Parser");

    Parser p(buffer, bufferSize, names, allocator, options);

    try
    {
        AstExpr* expr = p.parseExpr();
        size_t lines = p.lexer.current().location.end.line + (bufferSize > 0 && buffer[bufferSize - 1] != '\n');

        return ParseExprResult{
            expr, lines, std::move(p.hotcomments), std::move(p.parseErrors), std::move(p.commentLocations), std::move(p.cstNodeMap)
        };
    }
    catch (ParseError& err)
    {
        // when catching a fatal error, append it to the list of non-fatal errors and return
        p.parseErrors.push_back(err);

        return ParseExprResult{nullptr, 0, {}, p.parseErrors, {}, std::move(p.cstNodeMap)};
    }
}

Parser::Parser(const char* buffer, size_t bufferSize, AstNameTable& names, Allocator& allocator, const ParseOptions& options)
    : options(options)
    , lexer(buffer, bufferSize, names, options.parseFragment ? options.parseFragment->resumePosition : Position(0, 0))
    , allocator(allocator)
    , recursionCounter(0)
    , endMismatchSuspect(Lexeme(Location(), Lexeme::Eof))
    , localMap(AstName())
    , cstNodeMap(nullptr)
{
    Function top;
    top.vararg = true;

    functionStack.reserve(8);
    functionStack.push_back(top);

    nameSelf = names.getOrAdd("self");
    nameNumber = names.getOrAdd("number");
    nameError = names.getOrAdd(kParseNameError);
    nameNil = names.getOrAdd("nil"); // nil is a reserved keyword

    matchRecoveryStopOnToken.assign(Lexeme::Type::Reserved_END, 0);
    matchRecoveryStopOnToken[Lexeme::Type::Eof] = 1;

    // required for lookahead() to work across a comment boundary and for nextLexeme() to work when captureComments is false
    lexer.setSkipComments(true);

    // read first lexeme (any hot comments get .header = true)
    LUAU_ASSERT(hotcommentHeader);
    nextLexeme();

    // all hot comments parsed after the first non-comment lexeme are special in that they don't affect type checking / linting mode
    hotcommentHeader = false;

    // preallocate some buffers that are very likely to grow anyway; this works around std::vector's inefficient growth policy for small arrays
    localStack.reserve(16);
    scratchStat.reserve(16);
    scratchExpr.reserve(16);
    scratchLocal.reserve(16);
    scratchBinding.reserve(16);

    if (options.parseFragment)
    {
        localMap = options.parseFragment->localMap;
        localStack = options.parseFragment->localStack;
    }
}

bool Parser::blockFollow(const Lexeme& l)
{
    return l.type == Lexeme::Eof || l.type == Lexeme::ReservedElse || l.type == Lexeme::ReservedElseif || l.type == Lexeme::ReservedEnd ||
           l.type == Lexeme::ReservedUntil;
}

AstStatBlock* Parser::parseChunk()
{
    AstStatBlock* result = parseBlock();

    if (lexer.current().type != Lexeme::Eof)
        expectAndConsumeFail(Lexeme::Eof, nullptr);

    return result;
}

// chunk ::= {stat [`;']} [laststat [`;']]
// block ::= chunk
AstStatBlock* Parser::parseBlock()
{
    unsigned int localsBegin = saveLocals();

    AstStatBlock* result = parseBlockNoScope();

    restoreLocals(localsBegin);

    return result;
}

static bool isStatLast(AstStat* stat)
{
    return stat->is<AstStatBreak>() || stat->is<AstStatContinue>() || stat->is<AstStatReturn>();
}

AstStatBlock* Parser::parseBlockNoScope()
{
    TempVector<AstStat*> body(scratchStat);

    const Position prevPosition = lexer.previousLocation().end;

    while (!blockFollow(lexer.current()))
    {
        unsigned int oldRecursionCount = recursionCounter;

        incrementRecursionCounter("block");

        AstStat* stat = parseStat();

        recursionCounter = oldRecursionCount;

        if (lexer.current().type == ';')
        {
            nextLexeme();
            stat->hasSemicolon = true;
            stat->location.end = lexer.previousLocation().end;
        }

        body.push_back(stat);

        if (isStatLast(stat))
            break;
    }

    const Location location = Location(prevPosition, lexer.current().location.begin);

    return allocator.alloc<AstStatBlock>(location, copy(body));
}

// stat ::=
// varlist `=' explist |
// functioncall |
// do block end |
// while exp do block end |
// repeat block until exp |
// if exp then block {elseif exp then block} [else block] end |
// for binding `=' exp `,' exp [`,' exp] do block end |
// for namelist in explist do block end |
// function funcname funcbody |
// attributes function funcname funcbody |
// local function Name funcbody |
// local attributes function Name funcbody |
// local namelist [`=' explist]
// laststat ::= return [explist] | break
AstStat* Parser::parseStat()
{
    // guess the type from the token type
    switch (lexer.current().type)
    {
    case Lexeme::ReservedIf:
        return parseIf();
    case Lexeme::ReservedWhile:
        return parseWhile();
    case Lexeme::ReservedDo:
        return parseDo();
    case Lexeme::ReservedFor:
        return parseFor();
    case Lexeme::ReservedRepeat:
        return parseRepeat();
    case Lexeme::ReservedFunction:
        return parseFunctionStat(AstArray<AstAttr*>({nullptr, 0}));
    case Lexeme::ReservedLocal:
        return parseLocal(AstArray<AstAttr*>({nullptr, 0}));
    case Lexeme::ReservedReturn:
        return parseReturn();
    case Lexeme::ReservedBreak:
        return parseBreak();
    case Lexeme::Attribute:
        return parseAttributeStat();
    default:;
    }

    Location start = lexer.current().location;

    // we need to disambiguate a few cases, primarily assignment (lvalue = ...) vs statements-that-are calls
    AstExpr* expr = parsePrimaryExpr(/* asStatement= */ true);

    if (expr->is<AstExprCall>())
        return allocator.alloc<AstStatExpr>(expr->location, expr);

    // if the next token is , or =, it's an assignment (, means it's an assignment with multiple variables)
    if (lexer.current().type == ',' || lexer.current().type == '=')
        return parseAssignment(expr);

    // if the next token is a compound assignment operator, it's a compound assignment (these don't support multiple variables)
    if (std::optional<AstExprBinary::Op> op = parseCompoundOp(lexer.current()))
        return parseCompoundAssignment(expr, *op);

    // we know this isn't a call or an assignment; therefore it must be a context-sensitive keyword such as `type` or `continue`
    AstName ident = getIdentifier(expr);

    if (ident == "type")
        return parseTypeAlias(expr->location, /* exported= */ false, expr->location.begin);

    if (ident == "export" && lexer.current().type == Lexeme::Name && AstName(lexer.current().name) == "type")
    {
        Position typeKeywordPosition = lexer.current().location.begin;
        nextLexeme();
        return parseTypeAlias(expr->location, /* exported= */ true, typeKeywordPosition);
    }

    if (ident == "continue")
        return parseContinue(expr->location);

    if (options.allowDeclarationSyntax)
    {
        if (ident == "declare")
            return parseDeclaration(expr->location, AstArray<AstAttr*>({nullptr, 0}));
    }

    // skip unexpected symbol if lexer couldn't advance at all (statements are parsed in a loop)
    if (start == lexer.current().location)
        nextLexeme();

    return reportStatError(expr->location, copy({expr}), {}, "Incomplete statement: expected assignment or a function call");
}

// if exp then block {elseif exp then block} [else block] end
AstStat* Parser::parseIf()
{
    Location start = lexer.current().location;

    nextLexeme(); // if / elseif

    AstExpr* cond = parseExpr();

    Lexeme matchThen = lexer.current();
    std::optional<Location> thenLocation;
    if (expectAndConsume(Lexeme::ReservedThen, "if statement"))
        thenLocation = matchThen.location;

    AstStatBlock* thenbody = parseBlock();

    AstStat* elsebody = nullptr;
    Location end = start;
    std::optional<Location> elseLocation;

    if (lexer.current().type == Lexeme::ReservedElseif)
    {
        thenbody->hasEnd = true;
        unsigned int oldRecursionCount = recursionCounter;
        incrementRecursionCounter("elseif");
        elseLocation = lexer.current().location;
        elsebody = parseIf();
        end = elsebody->location;
        recursionCounter = oldRecursionCount;
    }
    else
    {
        Lexeme matchThenElse = matchThen;

        if (lexer.current().type == Lexeme::ReservedElse)
        {
            thenbody->hasEnd = true;
            elseLocation = lexer.current().location;
            matchThenElse = lexer.current();
            nextLexeme();

            elsebody = parseBlock();
            elsebody->location.begin = matchThenElse.location.end;
        }

        end = lexer.current().location;

        bool hasEnd = expectMatchEndAndConsume(Lexeme::ReservedEnd, matchThenElse);

        if (elsebody)
        {
            if (AstStatBlock* elseBlock = elsebody->as<AstStatBlock>())
                elseBlock->hasEnd = hasEnd;
        }
        else
            thenbody->hasEnd = hasEnd;
    }

    return allocator.alloc<AstStatIf>(Location(start, end), cond, thenbody, elsebody, thenLocation, elseLocation);
}

// while exp do block end
AstStat* Parser::parseWhile()
{
    Location start = lexer.current().location;

    nextLexeme(); // while

    AstExpr* cond = parseExpr();

    Lexeme matchDo = lexer.current();
    bool hasDo = expectAndConsume(Lexeme::ReservedDo, "while loop");

    functionStack.back().loopDepth++;

    AstStatBlock* body = parseBlock();

    functionStack.back().loopDepth--;

    Location end = lexer.current().location;

    bool hasEnd = expectMatchEndAndConsume(Lexeme::ReservedEnd, matchDo);
    body->hasEnd = hasEnd;

    return allocator.alloc<AstStatWhile>(Location(start, end), cond, body, hasDo, matchDo.location);
}

// repeat block until exp
AstStat* Parser::parseRepeat()
{
    Location start = lexer.current().location;

    Lexeme matchRepeat = lexer.current();
    nextLexeme(); // repeat

    unsigned int localsBegin = saveLocals();

    functionStack.back().loopDepth++;

    AstStatBlock* body = parseBlockNoScope();

    functionStack.back().loopDepth--;

    Position untilPosition = lexer.current().location.begin;
    bool hasUntil = expectMatchEndAndConsume(Lexeme::ReservedUntil, matchRepeat);
    body->hasEnd = hasUntil;

    AstExpr* cond = parseExpr();

    restoreLocals(localsBegin);

    AstStatRepeat* node = allocator.alloc<AstStatRepeat>(Location(start, cond->location), cond, body, hasUntil);
    if (options.storeCstData)
        cstNodeMap[node] = allocator.alloc<CstStatRepeat>(untilPosition);
    return node;
}

// do block end
AstStat* Parser::parseDo()
{
    Location start = lexer.current().location;

    Lexeme matchDo = lexer.current();
    nextLexeme(); // do

    AstStatBlock* body = parseBlock();

    body->location.begin = start.begin;

    Location endLocation = lexer.current().location;
    body->hasEnd = expectMatchEndAndConsume(Lexeme::ReservedEnd, matchDo);
    if (body->hasEnd)
        body->location.end = endLocation.end;

    if (options.storeCstData)
        cstNodeMap[body] = allocator.alloc<CstStatDo>(endLocation.begin);

    return body;
}

// break
AstStat* Parser::parseBreak()
{
    Location start = lexer.current().location;

    nextLexeme(); // break

    if (functionStack.back().loopDepth == 0)
        return reportStatError(start, {}, copy<AstStat*>({allocator.alloc<AstStatBreak>(start)}), "break statement must be inside a loop");

    return allocator.alloc<AstStatBreak>(start);
}

// continue
AstStat* Parser::parseContinue(const Location& start)
{
    if (functionStack.back().loopDepth == 0)
        return reportStatError(start, {}, copy<AstStat*>({allocator.alloc<AstStatContinue>(start)}), "continue statement must be inside a loop");

    // note: the token is already parsed for us!

    return allocator.alloc<AstStatContinue>(start);
}

// for binding `=' exp `,' exp [`,' exp] do block end |
// for bindinglist in explist do block end |
AstStat* Parser::parseFor()
{
    Location start = lexer.current().location;

    nextLexeme(); // for

    Binding varname = parseBinding();

    if (lexer.current().type == '=')
    {
        Position equalsPosition = lexer.current().location.begin;
        nextLexeme();

        AstExpr* from = parseExpr();

        Position endCommaPosition = lexer.current().location.begin;
        expectAndConsume(',', "index range");

        AstExpr* to = parseExpr();

        std::optional<Position> stepCommaPosition = std::nullopt;
        AstExpr* step = nullptr;

        if (lexer.current().type == ',')
        {
            stepCommaPosition = lexer.current().location.begin;
            nextLexeme();

            step = parseExpr();
        }

        Lexeme matchDo = lexer.current();
        bool hasDo = expectAndConsume(Lexeme::ReservedDo, "for loop");

        unsigned int localsBegin = saveLocals();

        functionStack.back().loopDepth++;

        AstLocal* var = pushLocal(varname);

        AstStatBlock* body = parseBlock();

        functionStack.back().loopDepth--;

        restoreLocals(localsBegin);

        Location end = lexer.current().location;

        bool hasEnd = expectMatchEndAndConsume(Lexeme::ReservedEnd, matchDo);
        body->hasEnd = hasEnd;

        AstStatFor* node = allocator.alloc<AstStatFor>(Location(start, end), var, from, to, step, body, hasDo, matchDo.location);
        if (options.storeCstData)
            cstNodeMap[node] = allocator.alloc<CstStatFor>(varname.colonPosition, equalsPosition, endCommaPosition, stepCommaPosition);

        return node;
    }
    else
    {
        TempVector<Binding> names(scratchBinding);
        AstArray<Position> varsCommaPosition;
        names.push_back(varname);

        if (lexer.current().type == ',')
        {
            if (options.storeCstData)
            {
                Position initialCommaPosition = lexer.current().location.begin;
                nextLexeme();
                parseBindingList(names, false, &varsCommaPosition, &initialCommaPosition);
            }
            else
            {
                nextLexeme();

                parseBindingList(names);
            }
        }

        Location inLocation = lexer.current().location;
        bool hasIn = expectAndConsume(Lexeme::ReservedIn, "for loop");

        TempVector<AstExpr*> values(scratchExpr);
        TempVector<Position> valuesCommaPositions(scratchPosition);
        parseExprList(values, options.storeCstData ? &valuesCommaPositions : nullptr);

        Lexeme matchDo = lexer.current();
        bool hasDo = expectAndConsume(Lexeme::ReservedDo, "for loop");

        unsigned int localsBegin = saveLocals();

        functionStack.back().loopDepth++;

        TempVector<AstLocal*> vars(scratchLocal);

        for (size_t i = 0; i < names.size(); ++i)
            vars.push_back(pushLocal(names[i]));

        AstStatBlock* body = parseBlock();

        functionStack.back().loopDepth--;

        restoreLocals(localsBegin);

        Location end = lexer.current().location;

        bool hasEnd = expectMatchEndAndConsume(Lexeme::ReservedEnd, matchDo);
        body->hasEnd = hasEnd;

        AstStatForIn* node =
            allocator.alloc<AstStatForIn>(Location(start, end), copy(vars), copy(values), body, hasIn, inLocation, hasDo, matchDo.location);
        if (options.storeCstData)
        {
            cstNodeMap[node] = allocator.alloc<CstStatForIn>(extractAnnotationColonPositions(names), varsCommaPosition, copy(valuesCommaPositions));
        }
        return node;
    }
}

// funcname ::= Name {`.' Name} [`:' Name]
AstExpr* Parser::parseFunctionName(bool& hasself, AstName& debugname)
{
    if (lexer.current().type == Lexeme::Name)
        debugname = AstName(lexer.current().name);

    // parse funcname into a chain of indexing operators
    AstExpr* expr = parseNameExpr("function name");

    unsigned int oldRecursionCount = recursionCounter;

    while (lexer.current().type == '.')
    {
        Position opPosition = lexer.current().location.begin;
        nextLexeme();

        Name name = parseName("field name");

        // while we could concatenate the name chain, for now let's just write the short name
        debugname = name.name;

        expr = allocator.alloc<AstExprIndexName>(Location(expr->location, name.location), expr, name.name, name.location, opPosition, '.');

        // note: while the parser isn't recursive here, we're generating recursive structures of unbounded depth
        incrementRecursionCounter("function name");
    }

    recursionCounter = oldRecursionCount;

    // finish with :
    if (lexer.current().type == ':')
    {
        Position opPosition = lexer.current().location.begin;
        nextLexeme();

        Name name = parseName("method name");

        // while we could concatenate the name chain, for now let's just write the short name
        debugname = name.name;

        expr = allocator.alloc<AstExprIndexName>(Location(expr->location, name.location), expr, name.name, name.location, opPosition, ':');

        hasself = true;
    }

    return expr;
}

// function funcname funcbody
AstStat* Parser::parseFunctionStat(const AstArray<AstAttr*>& attributes)
{
    Location start = lexer.current().location;

    if (attributes.size > 0)
        start = attributes.data[0]->location;

    Lexeme matchFunction = lexer.current();
    nextLexeme();

    bool hasself = false;
    AstName debugname;
    AstExpr* expr = parseFunctionName(hasself, debugname);

    matchRecoveryStopOnToken[Lexeme::ReservedEnd]++;

    AstExprFunction* body = parseFunctionBody(hasself, matchFunction, debugname, nullptr, attributes).first;

    matchRecoveryStopOnToken[Lexeme::ReservedEnd]--;

    AstStatFunction* node = allocator.alloc<AstStatFunction>(Location(start, body->location), expr, body);
    if (options.storeCstData)
        cstNodeMap[node] = allocator.alloc<CstStatFunction>(matchFunction.location.begin);
    return node;
}


std::pair<bool, AstAttr::Type> Parser::validateAttribute_DEPRECATED(const char* attributeName, const TempVector<AstAttr*>& attributes)
{
    LUAU_ASSERT(!FFlag::LuauParseAttributeFixUninit);

    AstAttr::Type type;

    // check if the attribute name is valid

    bool found = false;

    for (int i = 0; kAttributeEntries[i].name; ++i)
    {
        found = !strcmp(attributeName, kAttributeEntries[i].name);
        if (found)
        {
            type = kAttributeEntries[i].type;
            break;
        }
    }

    if (!found)
    {
        if (strlen(attributeName) == 1)
            report(lexer.current().location, "Attribute name is missing");
        else
            report(lexer.current().location, "Invalid attribute '%s'", attributeName);
    }
    else
    {
        // check that attribute is not duplicated
        for (const AstAttr* attr : attributes)
        {
            if (attr->type == type)
            {
                report(lexer.current().location, "Cannot duplicate attribute '%s'", attributeName);
            }
        }
    }

    return {found, type};
}

std::optional<AstAttr::Type> Parser::validateAttribute(const char* attributeName, const TempVector<AstAttr*>& attributes)
{
    LUAU_ASSERT(FFlag::LuauParseAttributeFixUninit);

    // check if the attribute name is valid
    std::optional<AstAttr::Type> type;

    for (int i = 0; kAttributeEntries[i].name; ++i)
    {
        if (strcmp(attributeName, kAttributeEntries[i].name) == 0)
        {
            type = kAttributeEntries[i].type;
            break;
        }
    }

    if (!type)
    {
        if (strlen(attributeName) == 1)
            report(lexer.current().location, "Attribute name is missing");
        else
            report(lexer.current().location, "Invalid attribute '%s'", attributeName);
    }
    else
    {
        // check that attribute is not duplicated
        for (const AstAttr* attr : attributes)
        {
            if (attr->type == *type)
                report(lexer.current().location, "Cannot duplicate attribute '%s'", attributeName);
        }
    }

    return type;
}

// attribute ::= '@' NAME
void Parser::parseAttribute(TempVector<AstAttr*>& attributes)
{
    LUAU_ASSERT(lexer.current().type == Lexeme::Type::Attribute);

    Location loc = lexer.current().location;

    if (FFlag::LuauParseAttributeFixUninit)
    {
        const char* name = lexer.current().name;
        std::optional<AstAttr::Type> type = validateAttribute(name, attributes);

        nextLexeme();

        if (type)
            attributes.push_back(allocator.alloc<AstAttr>(loc, *type));
    }
    else
    {
        const char* name = lexer.current().name;
        const auto [found, type] = validateAttribute_DEPRECATED(name, attributes);

        nextLexeme();

        if (found)
            attributes.push_back(allocator.alloc<AstAttr>(loc, type));
    }
}

// attributes ::= {attribute}
AstArray<AstAttr*> Parser::parseAttributes()
{
    Lexeme::Type type = lexer.current().type;

    LUAU_ASSERT(type == Lexeme::Attribute);

    TempVector<AstAttr*> attributes(scratchAttr);

    while (lexer.current().type == Lexeme::Attribute)
        parseAttribute(attributes);

    return copy(attributes);
}

// attributes local function Name funcbody
// attributes function funcname funcbody
// attributes `declare function' Name`(' [parlist] `)' [`:` Type]
// declare Name '{' Name ':' attributes `(' [parlist] `)' [`:` Type] '}'
AstStat* Parser::parseAttributeStat()
{
    AstArray<AstAttr*> attributes = parseAttributes();

    Lexeme::Type type = lexer.current().type;

    switch (type)
    {
    case Lexeme::Type::ReservedFunction:
        return parseFunctionStat(attributes);
    case Lexeme::Type::ReservedLocal:
        return parseLocal(attributes);
    case Lexeme::Type::Name:
        if (options.allowDeclarationSyntax && !strcmp("declare", lexer.current().data))
        {
            AstExpr* expr = parsePrimaryExpr(/* asStatement= */ true);
            return parseDeclaration(expr->location, attributes);
        }
        [[fallthrough]];
    default:
        return reportStatError(
            lexer.current().location,
            {},
            {},
            "Expected 'function', 'local function', 'declare function' or a function type declaration after attribute, but got %s instead",
            lexer.current().toString().c_str()
        );
    }
}

// local function Name funcbody |
// local bindinglist [`=' explist]
AstStat* Parser::parseLocal(const AstArray<AstAttr*>& attributes)
{
    Location start = lexer.current().location;

    if (attributes.size > 0)
        start = attributes.data[0]->location;

    Position localKeywordPosition = lexer.current().location.begin;
    nextLexeme(); // local

    if (lexer.current().type == Lexeme::ReservedFunction)
    {
        Lexeme matchFunction = lexer.current();
        nextLexeme();

        Position functionKeywordPosition = matchFunction.location.begin;
        // matchFunction is only used for diagnostics; to make it suitable for detecting missed indentation between
        // `local function` and `end`, we patch the token to begin at the column where `local` starts
        if (matchFunction.location.begin.line == start.begin.line)
            matchFunction.location.begin.column = start.begin.column;

        Name name = parseName("variable name");

        matchRecoveryStopOnToken[Lexeme::ReservedEnd]++;

        auto [body, var] = parseFunctionBody(false, matchFunction, name.name, &name, attributes);

        matchRecoveryStopOnToken[Lexeme::ReservedEnd]--;

        Location location{start.begin, body->location.end};

        AstStatLocalFunction* node = allocator.alloc<AstStatLocalFunction>(location, var, body);
        if (options.storeCstData)
            cstNodeMap[node] = allocator.alloc<CstStatLocalFunction>(localKeywordPosition, functionKeywordPosition);
        return node;
    }
    else
    {
        if (attributes.size != 0)
        {
            return reportStatError(
                lexer.current().location,
                {},
                {},
                "Expected 'function' after local declaration with attribute, but got %s instead",
                lexer.current().toString().c_str()
            );
        }

        matchRecoveryStopOnToken['=']++;

        TempVector<Binding> names(scratchBinding);
        AstArray<Position> varsCommaPositions;
        if (options.storeCstData)
            parseBindingList(names, false, &varsCommaPositions);
        else
            parseBindingList(names);

        matchRecoveryStopOnToken['=']--;

        TempVector<AstLocal*> vars(scratchLocal);

        TempVector<AstExpr*> values(scratchExpr);
        TempVector<Position> valuesCommaPositions(scratchPosition);

        std::optional<Location> equalsSignLocation;

        if (lexer.current().type == '=')
        {
            equalsSignLocation = lexer.current().location;

            nextLexeme();

            parseExprList(values, options.storeCstData ? &valuesCommaPositions : nullptr);
        }

        for (size_t i = 0; i < names.size(); ++i)
            vars.push_back(pushLocal(names[i]));

        Location end = values.empty() ? lexer.previousLocation() : values.back()->location;

        AstStatLocal* node = allocator.alloc<AstStatLocal>(Location(start, end), copy(vars), copy(values), equalsSignLocation);
        if (options.storeCstData)
        {
            cstNodeMap[node] = allocator.alloc<CstStatLocal>(extractAnnotationColonPositions(names), varsCommaPositions, copy(valuesCommaPositions));
        }

        return node;
    }
}

// return [explist]
AstStat* Parser::parseReturn()
{
    Location start = lexer.current().location;

    nextLexeme();

    TempVector<AstExpr*> list(scratchExpr);
    TempVector<Position> commaPositions(scratchPosition);

    if (!blockFollow(lexer.current()) && lexer.current().type != ';')
        parseExprList(list, options.storeCstData ? &commaPositions : nullptr);

    Location end = list.empty() ? start : list.back()->location;

    AstStatReturn* node = allocator.alloc<AstStatReturn>(Location(start, end), copy(list));
    if (options.storeCstData)
        cstNodeMap[node] = allocator.alloc<CstStatReturn>(copy(commaPositions));
    return node;
}

// type Name [`<' varlist `>'] `=' Type
AstStat* Parser::parseTypeAlias(const Location& start, bool exported, Position typeKeywordPosition)
{
    // parsing a type function
    if (lexer.current().type == Lexeme::ReservedFunction)
        return parseTypeFunction(start, exported, typeKeywordPosition);

    // parsing a type alias

    // note: `type` token is already parsed for us, so we just need to parse the rest

    std::optional<Name> name = parseNameOpt("type name");

    // Use error name if the name is missing
    if (!name)
        name = Name(nameError, lexer.current().location);

    Position genericsOpenPosition{0, 0};
    AstArray<Position> genericsCommaPositions;
    Position genericsClosePosition{0, 0};
    auto [generics, genericPacks] = options.storeCstData
                                        ? parseGenericTypeList(
                                              /* withDefaultValues= */ true, &genericsOpenPosition, &genericsCommaPositions, &genericsClosePosition
                                          )
                                        : parseGenericTypeList(/* withDefaultValues= */ true);

    Position equalsPosition = lexer.current().location.begin;
    expectAndConsume('=', "type alias");

    AstType* type = parseType();

    AstStatTypeAlias* node =
        allocator.alloc<AstStatTypeAlias>(Location(start, type->location), name->name, name->location, generics, genericPacks, type, exported);
    if (options.storeCstData)
        cstNodeMap[node] = allocator.alloc<CstStatTypeAlias>(
            typeKeywordPosition, genericsOpenPosition, genericsCommaPositions, genericsClosePosition, equalsPosition
        );
    return node;
}

// type function Name `(' arglist `)' `=' funcbody `end'
AstStat* Parser::parseTypeFunction(const Location& start, bool exported, Position typeKeywordPosition)
{
    Lexeme matchFn = lexer.current();
    nextLexeme();

    size_t errorsAtStart = parseErrors.size();

    // parse the name of the type function
    std::optional<Name> fnName = parseNameOpt("type function name");
    if (!fnName)
        fnName = Name(nameError, lexer.current().location);

    matchRecoveryStopOnToken[Lexeme::ReservedEnd]++;

    size_t oldTypeFunctionDepth = typeFunctionDepth;
    typeFunctionDepth = functionStack.size();

    AstExprFunction* body = parseFunctionBody(/* hasself */ false, matchFn, fnName->name, nullptr, AstArray<AstAttr*>({nullptr, 0})).first;

    typeFunctionDepth = oldTypeFunctionDepth;

    matchRecoveryStopOnToken[Lexeme::ReservedEnd]--;

    bool hasErrors = parseErrors.size() > errorsAtStart;

    AstStatTypeFunction* node =
        allocator.alloc<AstStatTypeFunction>(Location(start, body->location), fnName->name, fnName->location, body, exported, hasErrors);
    if (options.storeCstData)
        cstNodeMap[node] = allocator.alloc<CstStatTypeFunction>(typeKeywordPosition, matchFn.location.begin);
    return node;
}

AstDeclaredExternTypeProperty Parser::parseDeclaredExternTypeMethod(const AstArray<AstAttr*>& attributes)
{
    Location start = lexer.current().location;

    nextLexeme();

    Name fnName = parseName("function name");

    // TODO: generic method declarations CLI-39909
    AstArray<AstGenericType*> generics;
    AstArray<AstGenericTypePack*> genericPacks;
    generics.size = 0;
    generics.data = nullptr;
    genericPacks.size = 0;
    genericPacks.data = nullptr;

    MatchLexeme matchParen = lexer.current();
    expectAndConsume('(', "function parameter list start");

    TempVector<Binding> args(scratchBinding);

    bool vararg = false;
    Location varargLocation;
    AstTypePack* varargAnnotation = nullptr;
    if (lexer.current().type != ')')
        std::tie(vararg, varargLocation, varargAnnotation) = parseBindingList(args, /* allowDot3 */ true);

    expectMatchAndConsume(')', matchParen);

    AstTypePack* retTypes = parseOptionalReturnType();
    if (!retTypes)
        retTypes = allocator.alloc<AstTypePackExplicit>(lexer.current().location, AstTypeList{copy<AstType*>(nullptr, 0), nullptr});
    Location end = lexer.previousLocation();

    TempVector<AstType*> vars(scratchType);
    TempVector<std::optional<AstArgumentName>> varNames(scratchOptArgName);

    if (args.size() == 0 || args[0].name.name != "self" || args[0].annotation != nullptr)
    {
        return AstDeclaredExternTypeProperty{
            fnName.name, fnName.location, reportTypeError(Location(start, end), {}, "'self' must be present as the unannotated first parameter"), true
        };
    }

    // Skip the first index.
    for (size_t i = 1; i < args.size(); ++i)
    {
        varNames.push_back(AstArgumentName{args[i].name.name, args[i].name.location});

        if (args[i].annotation)
            vars.push_back(args[i].annotation);
        else
            vars.push_back(reportTypeError(Location(start, end), {}, "All declaration parameters aside from 'self' must be annotated"));
    }

    if (vararg && !varargAnnotation)
        report(start, "All declaration parameters aside from 'self' must be annotated");

    AstType* fnType = allocator.alloc<AstTypeFunction>(
        Location(start, end), attributes, generics, genericPacks, AstTypeList{copy(vars), varargAnnotation}, copy(varNames), retTypes
    );

    return AstDeclaredExternTypeProperty{fnName.name, fnName.location, fnType, true, Location(start, end)};
}

AstStat* Parser::parseDeclaration(const Location& start, const AstArray<AstAttr*>& attributes)
{
    // `declare` token is already parsed at this point

    if ((attributes.size != 0) && (lexer.current().type != Lexeme::ReservedFunction))
        return reportStatError(
            lexer.current().location,
            {},
            {},
            "Expected a function type declaration after attribute, but got %s instead",
            lexer.current().toString().c_str()
        );

    if (lexer.current().type == Lexeme::ReservedFunction)
    {
        nextLexeme();

        Name globalName = parseName("global function name");
        auto [generics, genericPacks] = parseGenericTypeList(/* withDefaultValues= */ false);

        MatchLexeme matchParen = lexer.current();

        expectAndConsume('(', "global function declaration");

        TempVector<Binding> args(scratchBinding);

        bool vararg = false;
        Location varargLocation;
        AstTypePack* varargAnnotation = nullptr;

        if (lexer.current().type != ')')
            std::tie(vararg, varargLocation, varargAnnotation) = parseBindingList(args, /* allowDot3= */ true);

        expectMatchAndConsume(')', matchParen);

        AstTypePack* retTypes;
        retTypes = parseOptionalReturnType();
        if (!retTypes)
            retTypes = allocator.alloc<AstTypePackExplicit>(lexer.current().location, AstTypeList{copy<AstType*>(nullptr, 0), nullptr});
        Location end = lexer.current().location;

        TempVector<AstType*> vars(scratchType);
        TempVector<AstArgumentName> varNames(scratchArgName);

        for (size_t i = 0; i < args.size(); ++i)
        {
            if (!args[i].annotation)
                return reportStatError(Location(start, end), {}, {}, "All declaration parameters must be annotated");

            vars.push_back(args[i].annotation);
            varNames.push_back({args[i].name.name, args[i].name.location});
        }

        if (vararg && !varargAnnotation)
            return reportStatError(Location(start, end), {}, {}, "All declaration parameters must be annotated");

        return allocator.alloc<AstStatDeclareFunction>(
            Location(start, end),
            attributes,
            globalName.name,
            globalName.location,
            generics,
            genericPacks,
            AstTypeList{copy(vars), varargAnnotation},
            copy(varNames),
            vararg,
            varargLocation,
            retTypes
        );
    }
    else if (AstName(lexer.current().name) == "class" || (FFlag::LuauDeclareExternType && AstName(lexer.current().name) == "extern"))
    {
        bool foundExtern = false;
        if (FFlag::LuauDeclareExternType)
        {
            if (AstName(lexer.current().name) == "extern")
            {
                foundExtern = true;
                nextLexeme();
                if (AstName(lexer.current().name) != "type")
                    return reportStatError(
                        lexer.current().location, {}, {}, "Expected `type` keyword after `extern`, but got %s instead", lexer.current().name
                    );
            }
        }

        nextLexeme();

        Location classStart = lexer.current().location;
        Name className = parseName(FFlag::LuauDeclareExternType ? "type name" : "class name");
        std::optional<AstName> superName = std::nullopt;

        if (AstName(lexer.current().name) == "extends")
        {
            nextLexeme();
            superName = parseName(FFlag::LuauDeclareExternType ? "supertype name" : "superclass name").name;
        }

        if (FFlag::LuauDeclareExternType)
        {
            if (foundExtern)
            {
                if (AstName(lexer.current().name) != "with")
                    report(
                        lexer.current().location,
                        "Expected `with` keyword before listing properties of the external type, but got %s instead",
                        lexer.current().name
                    );
                else
                    nextLexeme();
            }
        }

        TempVector<AstDeclaredExternTypeProperty> props(scratchDeclaredClassProps);
        AstTableIndexer* indexer = nullptr;

        while (lexer.current().type != Lexeme::ReservedEnd)
        {
            AstArray<AstAttr*> attributes{nullptr, 0};

            if (lexer.current().type == Lexeme::Attribute)
            {
                attributes = Parser::parseAttributes();

                if (lexer.current().type != Lexeme::ReservedFunction)
                    return reportStatError(
                        lexer.current().location,
                        {},
                        {},
                        "Expected a method type declaration after attribute, but got %s instead",
                        lexer.current().toString().c_str()
                    );
            }

            if (FFlag::LuauParseStringIndexer)
            {
                // There are two possibilities: Either it's a property or a function.
                if (lexer.current().type == Lexeme::ReservedFunction)
                {
                    props.push_back(parseDeclaredExternTypeMethod(attributes));
                }
                else if (lexer.current().type == '[')
                {
                    const Lexeme begin = lexer.current();
                    nextLexeme(); // [

                    if ((lexer.current().type == Lexeme::RawString || lexer.current().type == Lexeme::QuotedString) && lexer.lookahead().type == ']')
                    {
                        const Location nameBegin = lexer.current().location;
                        std::optional<AstArray<char>> chars = parseCharArray();

                        const Location nameEnd = lexer.previousLocation();

                        expectMatchAndConsume(']', begin);
                        expectAndConsume(':', "property type annotation");
                        AstType* type = parseType();

                        // since AstName contains a char*, it can't contain null
                        bool containsNull = chars && (memchr(chars->data, 0, chars->size) != nullptr);

                        if (chars && !containsNull)
                        {
                            props.push_back(AstDeclaredExternTypeProperty{
                                AstName(chars->data), Location(nameBegin, nameEnd), type, false, Location(begin.location, lexer.previousLocation())
                            });
                        }
                        else
                        {
                            report(begin.location, "String literal contains malformed escape sequence or \\0");
                        }
                    }
                    else if (indexer)
                    {
                        // maybe we don't need to parse the entire badIndexer...
                        // however, we either have { or [ to lint, not the entire table type or the bad indexer.
                        AstTableIndexer* badIndexer = parseTableIndexer(AstTableAccess::ReadWrite, std::nullopt, begin).node;

                        // we lose all additional indexer expressions from the AST after error recovery here
                        if (FFlag::LuauDeclareExternType)
                            report(badIndexer->location, "Cannot have more than one indexer on an extern type");
                        else
                            report(badIndexer->location, "Cannot have more than one class indexer");
                    }
                    else
                    {
                        indexer = parseTableIndexer(AstTableAccess::ReadWrite, std::nullopt, begin).node;
                    }
                }
                else
                {
                    Location propStart = lexer.current().location;
                    std::optional<Name> propName = parseNameOpt("property name");

                    if (!propName)
                        break;

                    expectAndConsume(':', "property type annotation");
                    AstType* propType = parseType();
                    props.push_back(AstDeclaredExternTypeProperty{
                        propName->name, propName->location, propType, false, Location(propStart, lexer.previousLocation())
                    });
                }
            }
            else
            {
                // There are two possibilities: Either it's a property or a function.
                if (lexer.current().type == Lexeme::ReservedFunction)
                {
                    props.push_back(parseDeclaredExternTypeMethod(attributes));
                }
                else if (lexer.current().type == '[' &&
                         (lexer.lookahead().type == Lexeme::RawString || lexer.lookahead().type == Lexeme::QuotedString))
                {
                    const Lexeme begin = lexer.current();
                    nextLexeme(); // [

                    const Location nameBegin = lexer.current().location;
                    std::optional<AstArray<char>> chars = parseCharArray();

                    const Location nameEnd = lexer.previousLocation();

                    expectMatchAndConsume(']', begin);
                    expectAndConsume(':', "property type annotation");
                    AstType* type = parseType();

                    // since AstName contains a char*, it can't contain null
                    bool containsNull = chars && (memchr(chars->data, 0, chars->size) != nullptr);

                    if (chars && !containsNull)
                    {
                        props.push_back(AstDeclaredExternTypeProperty{
                            AstName(chars->data), Location(nameBegin, nameEnd), type, false, Location(begin.location, lexer.previousLocation())
                        });
                    }
                    else
                    {
                        report(begin.location, "String literal contains malformed escape sequence or \\0");
                    }
                }
                else if (lexer.current().type == '[')
                {
                    if (indexer)
                    {
                        // maybe we don't need to parse the entire badIndexer...
                        // however, we either have { or [ to lint, not the entire table type or the bad indexer.
                        AstTableIndexer* badIndexer = parseTableIndexer(AstTableAccess::ReadWrite, std::nullopt, lexer.current()).node;

                        // we lose all additional indexer expressions from the AST after error recovery here
                        if (FFlag::LuauDeclareExternType)
                            report(badIndexer->location, "Cannot have more than one indexer on an extern type");
                        else
                            report(badIndexer->location, "Cannot have more than one class indexer");
                    }
                    else
                    {
                        indexer = parseTableIndexer(AstTableAccess::ReadWrite, std::nullopt, lexer.current()).node;
                    }
                }
                else
                {
                    Location propStart = lexer.current().location;
                    std::optional<Name> propName = parseNameOpt("property name");

                    if (!propName)
                        break;

                    expectAndConsume(':', "property type annotation");
                    AstType* propType = parseType();
                    props.push_back(AstDeclaredExternTypeProperty{
                        propName->name, propName->location, propType, false, Location(propStart, lexer.previousLocation())
                    });
                }
            }
        }

        Location classEnd = lexer.current().location;
        nextLexeme(); // skip past `end`

        return allocator.alloc<AstStatDeclareExternType>(Location(classStart, classEnd), className.name, superName, copy(props), indexer);
    }
    else if (std::optional<Name> globalName = parseNameOpt("global variable name"))
    {
        expectAndConsume(':', "global variable declaration");

        AstType* type = parseType(/* in declaration context */ true);
        return allocator.alloc<AstStatDeclareGlobal>(Location(start, type->location), globalName->name, globalName->location, type);
    }
    else if (FFlag::LuauDeclareExternType)
    {
        return reportStatError(start, {}, {}, "declare must be followed by an identifier, 'function', or 'extern type'");
    }
    else
    {
        return reportStatError(start, {}, {}, "declare must be followed by an identifier, 'function', or 'class'");
    }
}

static bool isExprLValue(AstExpr* expr)
{
    return expr->is<AstExprLocal>() || expr->is<AstExprGlobal>() || expr->is<AstExprIndexExpr>() || expr->is<AstExprIndexName>();
}

// varlist `=' explist
AstStat* Parser::parseAssignment(AstExpr* initial)
{
    if (!isExprLValue(initial))
        initial = reportExprError(initial->location, copy({initial}), "Assigned expression must be a variable or a field");

    TempVector<AstExpr*> vars(scratchExpr);
    TempVector<Position> varsCommaPositions(scratchPosition);
    vars.push_back(initial);

    while (lexer.current().type == ',')
    {
        if (options.storeCstData)
            varsCommaPositions.push_back(lexer.current().location.begin);
        nextLexeme();

        AstExpr* expr = parsePrimaryExpr(/* asStatement= */ true);

        if (!isExprLValue(expr))
            expr = reportExprError(expr->location, copy({expr}), "Assigned expression must be a variable or a field");

        vars.push_back(expr);
    }

    Position equalsPosition = lexer.current().location.begin;
    expectAndConsume('=', "assignment");

    TempVector<AstExpr*> values(scratchExprAux);
    TempVector<Position> valuesCommaPositions(scratchPosition);
    parseExprList(values, options.storeCstData ? &valuesCommaPositions : nullptr);

    AstStatAssign* node = allocator.alloc<AstStatAssign>(Location(initial->location, values.back()->location), copy(vars), copy(values));
    cstNodeMap[node] = allocator.alloc<CstStatAssign>(copy(varsCommaPositions), equalsPosition, copy(valuesCommaPositions));
    return node;
}

// var [`+=' | `-=' | `*=' | `/=' | `%=' | `^=' | `..='] exp
AstStat* Parser::parseCompoundAssignment(AstExpr* initial, AstExprBinary::Op op)
{
    if (!isExprLValue(initial))
    {
        initial = reportExprError(initial->location, copy({initial}), "Assigned expression must be a variable or a field");
    }

    Position opPosition = lexer.current().location.begin;
    nextLexeme();

    AstExpr* value = parseExpr();

    AstStatCompoundAssign* node = allocator.alloc<AstStatCompoundAssign>(Location(initial->location, value->location), op, initial, value);
    if (options.storeCstData)
        cstNodeMap[node] = allocator.alloc<CstStatCompoundAssign>(opPosition);
    return node;
}

std::pair<AstLocal*, AstArray<AstLocal*>> Parser::prepareFunctionArguments(const Location& start, bool hasself, const TempVector<Binding>& args)
{
    AstLocal* self = nullptr;

    if (hasself)
        self = pushLocal(Binding(Name(nameSelf, start), nullptr));

    TempVector<AstLocal*> vars(scratchLocal);

    for (size_t i = 0; i < args.size(); ++i)
        vars.push_back(pushLocal(args[i]));

    return {self, copy(vars)};
}

// funcbody ::= `(' [parlist] `)' [`:' ReturnType] block end
// parlist ::= bindinglist [`,' `...'] | `...'
std::pair<AstExprFunction*, AstLocal*> Parser::parseFunctionBody(
    bool hasself,
    const Lexeme& matchFunction,
    const AstName& debugname,
    const Name* localName,
    const AstArray<AstAttr*>& attributes
)
{
    Location start = matchFunction.location;

    if (attributes.size > 0)
        start = attributes.data[0]->location;

    auto* cstNode = options.storeCstData ? allocator.alloc<CstExprFunction>() : nullptr;

    auto [generics, genericPacks] =
        cstNode
            ? parseGenericTypeList(
                  /* withDefaultValues= */ false, &cstNode->openGenericsPosition, &cstNode->genericsCommaPositions, &cstNode->closeGenericsPosition
              )
            : parseGenericTypeList(/* withDefaultValues= */ false);

    MatchLexeme matchParen = lexer.current();
    expectAndConsume('(', "function");

    // NOTE: This was added in conjunction with passing `searchForMissing` to
    // `expectMatchAndConsume` inside `parseTableType` so that the behavior of
    // parsing code like below (note the missing `}`):
    //
    //  function (t: { a: number  ) end
    //
    // ... will still parse as (roughly):
    //
    //  function (t: { a: number }) end
    //
    matchRecoveryStopOnToken[')']++;

    TempVector<Binding> args(scratchBinding);

    bool vararg = false;
    Location varargLocation;
    AstTypePack* varargAnnotation = nullptr;

    if (lexer.current().type != ')')
    {
        if (cstNode)
            std::tie(vararg, varargLocation, varargAnnotation) =
                parseBindingList(args, /* allowDot3= */ true, &cstNode->argsCommaPositions, nullptr, &cstNode->varargAnnotationColonPosition);
        else
            std::tie(vararg, varargLocation, varargAnnotation) = parseBindingList(args, /* allowDot3= */ true);
    }

    std::optional<Location> argLocation;

    if (matchParen.type == Lexeme::Type('(') && lexer.current().type == Lexeme::Type(')'))
        argLocation = Location(matchParen.position, lexer.current().location.end);

    expectMatchAndConsume(')', matchParen, true);

    matchRecoveryStopOnToken[')']--;

    AstTypePack* typelist = parseOptionalReturnType(cstNode ? &cstNode->returnSpecifierPosition : nullptr);

    AstLocal* funLocal = nullptr;

    if (localName)
        funLocal = pushLocal(Binding(*localName, nullptr));

    unsigned int localsBegin = saveLocals();

    Function fun;
    fun.vararg = vararg;

    functionStack.emplace_back(fun);

    auto [self, vars] = prepareFunctionArguments(start, hasself, args);

    AstStatBlock* body = parseBlock();

    functionStack.pop_back();

    restoreLocals(localsBegin);

    Location end = lexer.current().location;

    bool hasEnd = expectMatchEndAndConsume(Lexeme::ReservedEnd, matchFunction);
    body->hasEnd = hasEnd;

    AstExprFunction* node = allocator.alloc<AstExprFunction>(
        Location(start, end),
        attributes,
        generics,
        genericPacks,
        self,
        vars,
        vararg,
        varargLocation,
        body,
        functionStack.size(),
        debugname,
        typelist,
        varargAnnotation,
        argLocation
    );
    if (options.storeCstData)
    {
        cstNode->functionKeywordPosition = matchFunction.location.begin;
        cstNode->argsAnnotationColonPositions = extractAnnotationColonPositions(args);
        cstNodeMap[node] = cstNode;
    }

    return {node, funLocal};
}

// explist ::= {exp `,'} exp
void Parser::parseExprList(TempVector<AstExpr*>& result, TempVector<Position>* commaPositions)
{
    result.push_back(parseExpr());

    while (lexer.current().type == ',')
    {
        if (commaPositions)
            commaPositions->push_back(lexer.current().location.begin);
        nextLexeme();

        if (lexer.current().type == ')')
        {
            report(lexer.current().location, "Expected expression after ',' but got ')' instead");
            break;
        }

        result.push_back(parseExpr());
    }
}

Parser::Binding Parser::parseBinding()
{
    std::optional<Name> name = parseNameOpt("variable name");

    // Use placeholder if the name is missing
    if (!name)
        name = Name(nameError, lexer.current().location);

    Position colonPosition = lexer.current().location.begin;
    AstType* annotation = parseOptionalType();

    if (options.storeCstData)
        return Binding(*name, annotation, colonPosition);
    else
        return Binding(*name, annotation);
}

AstArray<Position> Parser::extractAnnotationColonPositions(const TempVector<Binding>& bindings)
{
    TempVector<Position> annotationColonPositions(scratchPosition);
    for (size_t i = 0; i < bindings.size(); ++i)
        annotationColonPositions.push_back(bindings[i].colonPosition);
    return copy(annotationColonPositions);
}

// bindinglist ::= (binding | `...') [`,' bindinglist]
std::tuple<bool, Location, AstTypePack*> Parser::parseBindingList(
    TempVector<Binding>& result,
    bool allowDot3,
    AstArray<Position>* commaPositions,
    Position* initialCommaPosition,
    Position* varargAnnotationColonPosition
)
{
    TempVector<Position> localCommaPositions(scratchPosition);

    if (commaPositions && initialCommaPosition)
        localCommaPositions.push_back(*initialCommaPosition);

    while (true)
    {
        if (lexer.current().type == Lexeme::Dot3 && allowDot3)
        {
            Location varargLocation = lexer.current().location;
            nextLexeme();

            AstTypePack* tailAnnotation = nullptr;
            if (lexer.current().type == ':')
            {
                if (varargAnnotationColonPosition)
                    *varargAnnotationColonPosition = lexer.current().location.begin;

                nextLexeme();
                tailAnnotation = parseVariadicArgumentTypePack();
            }

            if (commaPositions)
                *commaPositions = copy(localCommaPositions);

            return {true, varargLocation, tailAnnotation};
        }

        result.push_back(parseBinding());

        if (lexer.current().type != ',')
            break;
        if (commaPositions)
            localCommaPositions.push_back(lexer.current().location.begin);
        nextLexeme();
    }

    if (commaPositions)
        *commaPositions = copy(localCommaPositions);

    return {false, Location(), nullptr};
}

AstType* Parser::parseOptionalType()
{
    if (lexer.current().type == ':')
    {
        nextLexeme();
        return parseType();
    }
    else
        return nullptr;
}

// TypeList ::= Type [`,' TypeList] | ...Type
AstTypePack* Parser::parseTypeList(
    TempVector<AstType*>& result,
    TempVector<std::optional<AstArgumentName>>& resultNames,
    TempVector<Position>* commaPositions,
    TempVector<std::optional<Position>>* nameColonPositions
)
{
    while (true)
    {
        if (shouldParseTypePack(lexer))
            return parseTypePack();

        if (lexer.current().type == Lexeme::Name && lexer.lookahead().type == ':')
        {
            // Fill in previous argument names with empty slots
            while (resultNames.size() < result.size())
                resultNames.push_back({});
            if (nameColonPositions)
            {
                while (nameColonPositions->size() < result.size())
                    nameColonPositions->push_back({});
            }

            resultNames.push_back(AstArgumentName{AstName(lexer.current().name), lexer.current().location});
            nextLexeme();

            if (nameColonPositions)
                nameColonPositions->push_back(lexer.current().location.begin);
            expectAndConsume(':');
        }
        else if (!resultNames.empty())
        {
            // If we have a type with named arguments, provide elements for all types
            resultNames.push_back({});
            if (nameColonPositions)
                nameColonPositions->push_back({});
        }

        result.push_back(parseType());
        if (lexer.current().type != ',')
            break;

        if (commaPositions)
            commaPositions->push_back(lexer.current().location.begin);
        nextLexeme();

        if (lexer.current().type == ')')
        {
            report(lexer.current().location, "Expected type after ',' but got ')' instead");
            break;
        }
    }

    return nullptr;
}

AstTypePack* Parser::parseOptionalReturnType(Position* returnSpecifierPosition)
{
    if (lexer.current().type == ':' || lexer.current().type == Lexeme::SkinnyArrow)
    {
        if (lexer.current().type == Lexeme::SkinnyArrow)
            report(lexer.current().location, "Function return type annotations are written after ':' instead of '->'");

        if (returnSpecifierPosition)
            *returnSpecifierPosition = lexer.current().location.begin;
        nextLexeme();

        unsigned int oldRecursionCount = recursionCounter;

        auto result = parseReturnType();
        LUAU_ASSERT(result);

        // At this point, if we find a , character, it indicates that there are multiple return types
        // in this type annotation, but the list wasn't wrapped in parentheses.
        if (lexer.current().type == ',')
        {
            report(lexer.current().location, "Expected a statement, got ','; did you forget to wrap the list of return types in parentheses?");

            nextLexeme();
        }

        recursionCounter = oldRecursionCount;

        return result;
    }

    return nullptr;
}

// ReturnType ::= Type | `(' TypeList `)'
AstTypePack* Parser::parseReturnType()
{
    incrementRecursionCounter("type annotation");

    Lexeme begin = lexer.current();

    if (lexer.current().type != '(')
    {
        if (shouldParseTypePack(lexer))
        {
            return parseTypePack();
        }
        else
        {
            AstType* type = parseType();
            AstTypePackExplicit* node = allocator.alloc<AstTypePackExplicit>(type->location, AstTypeList{copy(&type, 1), nullptr});
            if (options.storeCstData)
                cstNodeMap[node] = allocator.alloc<CstTypePackExplicit>();
            return node;
        }
    }

    nextLexeme();

    matchRecoveryStopOnToken[Lexeme::SkinnyArrow]++;

    TempVector<AstType*> result(scratchType);
    TempVector<std::optional<AstArgumentName>> resultNames(scratchOptArgName);
    TempVector<Position> commaPositions(scratchPosition);
    TempVector<std::optional<Position>> nameColonPositions(scratchOptPosition);
    AstTypePack* varargAnnotation = nullptr;

    // possibly () -> ReturnType
    if (lexer.current().type != ')')
    {
        if (options.storeCstData)
            varargAnnotation = parseTypeList(result, resultNames, &commaPositions, &nameColonPositions);
        else
            varargAnnotation = parseTypeList(result, resultNames);
    }

    const Location location{begin.location, lexer.current().location};
    Position closeParenthesesPosition = lexer.current().location.begin;
    expectMatchAndConsume(')', begin, true);

    matchRecoveryStopOnToken[Lexeme::SkinnyArrow]--;

    if (lexer.current().type != Lexeme::SkinnyArrow && resultNames.empty())
    {
        // If it turns out that it's just '(A)', it's possible that there are unions/intersections to follow, so fold over it.
        if (result.size() == 1)
        {
            // TODO(CLI-140667): stop parsing type suffix when varargAnnotation != nullptr - this should be a parse error
            AstType* inner = varargAnnotation == nullptr ? allocator.alloc<AstTypeGroup>(location, result[0]) : result[0];
            AstType* returnType = parseTypeSuffix(inner, begin.location);

            if (DFFlag::DebugLuauReportReturnTypeVariadicWithTypeSuffix && varargAnnotation != nullptr &&
                (returnType->is<AstTypeUnion>() || returnType->is<AstTypeIntersection>()))
                luau_telemetry_parsed_return_type_variadic_with_type_suffix = true;

            // If parseType parses nothing, then returnType->location.end only points at the last non-type-pack
            // type to successfully parse.  We need the span of the whole annotation.
            Position endPos = result.size() == 1 ? location.end : returnType->location.end;

            AstTypePackExplicit* node =
                allocator.alloc<AstTypePackExplicit>(Location{location.begin, endPos}, AstTypeList{copy(&returnType, 1), varargAnnotation});
            if (options.storeCstData)
                cstNodeMap[node] = allocator.alloc<CstTypePackExplicit>();
            return node;
        }

        AstTypePackExplicit* node = allocator.alloc<AstTypePackExplicit>(location, AstTypeList{copy(result), varargAnnotation});
        if (options.storeCstData)
            cstNodeMap[node] = allocator.alloc<CstTypePackExplicit>(location.begin, closeParenthesesPosition, copy(commaPositions));
        return node;
    }

    Position returnArrowPosition = lexer.current().location.begin;
    AstType* tail = parseFunctionTypeTail(begin, {nullptr, 0}, {}, {}, copy(result), copy(resultNames), varargAnnotation);

    if (options.storeCstData && tail->is<AstTypeFunction>())
    {
        cstNodeMap[tail] = allocator.alloc<CstTypeFunction>(
            Position{0, 0},
            AstArray<Position>{},
            Position{0, 0},
            location.begin,
            copy(nameColonPositions),
            copy(commaPositions),
            closeParenthesesPosition,
            returnArrowPosition
        );
    }

    AstTypePackExplicit* node = allocator.alloc<AstTypePackExplicit>(Location{location, tail->location}, AstTypeList{copy(&tail, 1), nullptr});
    if (options.storeCstData)
        cstNodeMap[node] = allocator.alloc<CstTypePackExplicit>();
    return node;
}

std::pair<CstExprConstantString::QuoteStyle, unsigned int> Parser::extractStringDetails()
{
    CstExprConstantString::QuoteStyle style;
    unsigned int blockDepth = 0;

    switch (lexer.current().type)
    {
    case Lexeme::QuotedString:
        style =
            lexer.current().getQuoteStyle() == Lexeme::QuoteStyle::Double ? CstExprConstantString::QuotedDouble : CstExprConstantString::QuotedSingle;
        break;
    case Lexeme::InterpStringSimple:
        style = CstExprConstantString::QuotedInterp;
        break;
    case Lexeme::RawString:
    {
        style = CstExprConstantString::QuotedRaw;
        blockDepth = lexer.current().getBlockDepth();
        break;
    }
    default:
        LUAU_ASSERT(false && "Invalid string type");
    }

    return {style, blockDepth};
}

// TableIndexer ::= `[' Type `]' `:' Type
Parser::TableIndexerResult Parser::parseTableIndexer(AstTableAccess access, std::optional<Location> accessLocation, Lexeme begin)
{
    if (!FFlag::LuauParseStringIndexer)
    {
        begin = lexer.current();
        nextLexeme(); // [
    }

    AstType* index = parseType();

    Position indexerClosePosition = lexer.current().location.begin;
    expectMatchAndConsume(']', begin);

    Position colonPosition = lexer.current().location.begin;
    expectAndConsume(':', "table field");

    AstType* result = parseType();

    return {
        allocator.alloc<AstTableIndexer>(AstTableIndexer{index, result, Location(begin.location, result->location), access, accessLocation}),
        begin.location.begin,
        indexerClosePosition,
        colonPosition,
    };
}

// TableProp ::= Name `:' Type
// TablePropOrIndexer ::= TableProp | TableIndexer
// PropList ::= TablePropOrIndexer {fieldsep TablePropOrIndexer} [fieldsep]
// TableType ::= `{' PropList `}'
AstType* Parser::parseTableType(bool inDeclarationContext)
{
    incrementRecursionCounter("type annotation");

    TempVector<AstTableProp> props(scratchTableTypeProps);
    TempVector<CstTypeTable::Item> cstItems(scratchCstTableTypeProps);
    AstTableIndexer* indexer = nullptr;

    Location start = lexer.current().location;

    MatchLexeme matchBrace = lexer.current();
    expectAndConsume('{', "table type");

    bool isArray = false;

    while (lexer.current().type != '}')
    {
        AstTableAccess access = AstTableAccess::ReadWrite;
        std::optional<Location> accessLocation;

        if (lexer.current().type == Lexeme::Name && lexer.lookahead().type != ':')
        {
            if (AstName(lexer.current().name) == "read")
            {
                accessLocation = lexer.current().location;
                access = AstTableAccess::Read;
                lexer.next();
            }
            else if (AstName(lexer.current().name) == "write")
            {
                accessLocation = lexer.current().location;
                access = AstTableAccess::Write;
                lexer.next();
            }
        }

        if (FFlag::LuauParseStringIndexer)
        {
            if (lexer.current().type == '[')
            {
                const Lexeme begin = lexer.current();
                nextLexeme(); // [

                if ((lexer.current().type == Lexeme::RawString || lexer.current().type == Lexeme::QuotedString) && lexer.lookahead().type == ']')
                {
                    CstExprConstantString::QuoteStyle style;
                    unsigned int blockDepth = 0;
                    if (options.storeCstData)
                        std::tie(style, blockDepth) = extractStringDetails();

                    Position stringPosition = lexer.current().location.begin;
                    AstArray<char> sourceString;
                    std::optional<AstArray<char>> chars = parseCharArray(options.storeCstData ? &sourceString : nullptr);

                    Position indexerClosePosition = lexer.current().location.begin;
                    expectMatchAndConsume(']', begin);
                    Position colonPosition = lexer.current().location.begin;
                    expectAndConsume(':', "table field");

                    AstType* type = parseType();

                    // since AstName contains a char*, it can't contain null
                    bool containsNull = chars && (memchr(chars->data, 0, chars->size) != nullptr);

                    if (chars && !containsNull)
                    {
                        props.push_back(AstTableProp{AstName(chars->data), begin.location, type, access, accessLocation});
                        if (options.storeCstData)
                            cstItems.push_back(CstTypeTable::Item{
                                CstTypeTable::Item::Kind::StringProperty,
                                begin.location.begin,
                                indexerClosePosition,
                                colonPosition,
                                tableSeparator(),
                                lexer.current().location.begin,
                                allocator.alloc<CstExprConstantString>(sourceString, style, blockDepth),
                                stringPosition
                            });
                    }
                    else
                        report(begin.location, "String literal contains malformed escape sequence or \\0");
                }
                else
                {
                    if (indexer)
                    {
                        // maybe we don't need to parse the entire badIndexer...
                        // however, we either have { or [ to lint, not the entire table type or the bad indexer.
                        AstTableIndexer* badIndexer = parseTableIndexer(access, accessLocation, begin).node;

                        // we lose all additional indexer expressions from the AST after error recovery here
                        report(badIndexer->location, "Cannot have more than one table indexer");
                    }
                    else
                    {
                        auto tableIndexerResult = parseTableIndexer(access, accessLocation, begin);
                        indexer = tableIndexerResult.node;
                        if (options.storeCstData)
                            cstItems.push_back(CstTypeTable::Item{
                                CstTypeTable::Item::Kind::Indexer,
                                tableIndexerResult.indexerOpenPosition,
                                tableIndexerResult.indexerClosePosition,
                                tableIndexerResult.colonPosition,
                                tableSeparator(),
                                lexer.current().location.begin,
                            });
                    }
                }
            }
            else if (props.empty() && !indexer && !(lexer.current().type == Lexeme::Name && lexer.lookahead().type == ':'))
            {
                AstType* type = parseType();

                // array-like table type: {T} desugars into {[number]: T}
                isArray = true;
                AstType* index = allocator.alloc<AstTypeReference>(type->location, std::nullopt, nameNumber, std::nullopt, type->location);
                indexer = allocator.alloc<AstTableIndexer>(AstTableIndexer{index, type, type->location, access, accessLocation});

                break;
            }
            else
            {
                std::optional<Name> name = parseNameOpt("table field");

                if (!name)
                    break;

                Position colonPosition = lexer.current().location.begin;
                expectAndConsume(':', "table field");

                AstType* type = parseType(inDeclarationContext);

                props.push_back(AstTableProp{name->name, name->location, type, access, accessLocation});
                if (options.storeCstData)
                    cstItems.push_back(CstTypeTable::Item{
                        CstTypeTable::Item::Kind::Property,
                        Position{0, 0},
                        Position{0, 0},
                        colonPosition,
                        tableSeparator(),
                        lexer.current().location.begin
                    });
            }
        }
        else
        {
            if (lexer.current().type == '[' && (lexer.lookahead().type == Lexeme::RawString || lexer.lookahead().type == Lexeme::QuotedString))
            {
                const Lexeme begin = lexer.current();
                nextLexeme(); // [

                CstExprConstantString::QuoteStyle style;
                unsigned int blockDepth = 0;
                if (options.storeCstData)
                    std::tie(style, blockDepth) = extractStringDetails();

                Position stringPosition = lexer.current().location.begin;
                AstArray<char> sourceString;
                std::optional<AstArray<char>> chars = parseCharArray(options.storeCstData ? &sourceString : nullptr);

                Position indexerClosePosition = lexer.current().location.begin;
                expectMatchAndConsume(']', begin);
                Position colonPosition = lexer.current().location.begin;
                expectAndConsume(':', "table field");

                AstType* type = parseType();

                // since AstName contains a char*, it can't contain null
                bool containsNull = chars && (memchr(chars->data, 0, chars->size) != nullptr);

                if (chars && !containsNull)
                {
                    props.push_back(AstTableProp{AstName(chars->data), begin.location, type, access, accessLocation});
                    if (options.storeCstData)
                        cstItems.push_back(CstTypeTable::Item{
                            CstTypeTable::Item::Kind::StringProperty,
                            begin.location.begin,
                            indexerClosePosition,
                            colonPosition,
                            tableSeparator(),
                            lexer.current().location.begin,
                            allocator.alloc<CstExprConstantString>(sourceString, style, blockDepth),
                            stringPosition
                        });
                }
                else
                    report(begin.location, "String literal contains malformed escape sequence or \\0");
            }
            else if (lexer.current().type == '[')
            {
                if (indexer)
                {
                    // maybe we don't need to parse the entire badIndexer...
                    // however, we either have { or [ to lint, not the entire table type or the bad indexer.
                    AstTableIndexer* badIndexer = parseTableIndexer(access, accessLocation, lexer.current()).node;

                    // we lose all additional indexer expressions from the AST after error recovery here
                    report(badIndexer->location, "Cannot have more than one table indexer");
                }
                else
                {
                    // the last param in the parseTableIndexer is ignored
                    auto tableIndexerResult = parseTableIndexer(access, accessLocation, lexer.current());
                    indexer = tableIndexerResult.node;
                    if (options.storeCstData)
                        cstItems.push_back(CstTypeTable::Item{
                            CstTypeTable::Item::Kind::Indexer,
                            tableIndexerResult.indexerOpenPosition,
                            tableIndexerResult.indexerClosePosition,
                            tableIndexerResult.colonPosition,
                            tableSeparator(),
                            lexer.current().location.begin,
                        });
                }
            }
            else if (props.empty() && !indexer && !(lexer.current().type == Lexeme::Name && lexer.lookahead().type == ':'))
            {
                AstType* type = parseType();

                // array-like table type: {T} desugars into {[number]: T}
                isArray = true;
                AstType* index = allocator.alloc<AstTypeReference>(type->location, std::nullopt, nameNumber, std::nullopt, type->location);
                indexer = allocator.alloc<AstTableIndexer>(AstTableIndexer{index, type, type->location, access, accessLocation});

                break;
            }
            else
            {
                std::optional<Name> name = parseNameOpt("table field");

                if (!name)
                    break;

                Position colonPosition = lexer.current().location.begin;
                expectAndConsume(':', "table field");

                AstType* type = parseType(inDeclarationContext);

                props.push_back(AstTableProp{name->name, name->location, type, access, accessLocation});
                if (options.storeCstData)
                    cstItems.push_back(CstTypeTable::Item{
                        CstTypeTable::Item::Kind::Property,
                        Position{0, 0},
                        Position{0, 0},
                        colonPosition,
                        tableSeparator(),
                        lexer.current().location.begin
                    });
            }
        }

        if (lexer.current().type == ',' || lexer.current().type == ';')
        {
            nextLexeme();
        }
        else
        {
            if (lexer.current().type != '}')
                break;
        }
    }

    Location end = lexer.current().location;

    if (!expectMatchAndConsume('}', matchBrace, /* searchForMissing = */ true))
        end = lexer.previousLocation();

    AstTypeTable* node = allocator.alloc<AstTypeTable>(Location(start, end), copy(props), indexer);
    if (options.storeCstData)
        cstNodeMap[node] = allocator.alloc<CstTypeTable>(copy(cstItems), isArray);
    return node;
}

// ReturnType ::= Type | `(' TypeList `)'
// FunctionType ::= [`<' varlist `>'] `(' [TypeList] `)' `->` ReturnType
AstTypeOrPack Parser::parseFunctionType(bool allowPack, const AstArray<AstAttr*>& attributes)
{
    incrementRecursionCounter("type annotation");

    bool forceFunctionType = lexer.current().type == '<';

    Lexeme begin = lexer.current();

    Position genericsOpenPosition{0, 0};
    AstArray<Position> genericsCommaPositions;
    Position genericsClosePosition{0, 0};
    auto [generics, genericPacks] = options.storeCstData
                                        ? parseGenericTypeList(
                                              /* withDefaultValues= */ false, &genericsOpenPosition, &genericsCommaPositions, &genericsClosePosition
                                          )
                                        : parseGenericTypeList(/* withDefaultValues= */ false);

    Lexeme parameterStart = lexer.current();

    expectAndConsume('(', "function parameters");

    matchRecoveryStopOnToken[Lexeme::SkinnyArrow]++;

    TempVector<AstType*> params(scratchType);
    TempVector<std::optional<AstArgumentName>> names(scratchOptArgName);
    TempVector<std::optional<Position>> nameColonPositions(scratchOptPosition);
    TempVector<Position> argCommaPositions(scratchPosition);
    AstTypePack* varargAnnotation = nullptr;

    if (lexer.current().type != ')')
    {
        if (options.storeCstData)
            varargAnnotation = parseTypeList(params, names, &argCommaPositions, &nameColonPositions);
        else
            varargAnnotation = parseTypeList(params, names);
    }

    Location closeArgsLocation = lexer.current().location;
    expectMatchAndConsume(')', parameterStart, true);

    matchRecoveryStopOnToken[Lexeme::SkinnyArrow]--;

    AstArray<AstType*> paramTypes = copy(params);

    if (!names.empty())
        forceFunctionType = true;

    bool returnTypeIntroducer = lexer.current().type == Lexeme::SkinnyArrow || lexer.current().type == ':';

    // Not a function at all. Just a parenthesized type. Or maybe a type pack with a single element
    if (params.size() == 1 && !varargAnnotation && !forceFunctionType && !returnTypeIntroducer)
    {
        if (allowPack)
        {
            AstTypePackExplicit* node = allocator.alloc<AstTypePackExplicit>(begin.location, AstTypeList{paramTypes, nullptr});
            if (options.storeCstData)
                cstNodeMap[node] =
                    allocator.alloc<CstTypePackExplicit>(parameterStart.location.begin, closeArgsLocation.begin, copy(argCommaPositions));
            return {{}, node};
        }
        else
        {
            return {allocator.alloc<AstTypeGroup>(Location(parameterStart.location, closeArgsLocation), params[0]), {}};
        }
    }

    if (!forceFunctionType && !returnTypeIntroducer && allowPack)
    {
        AstTypePackExplicit* node = allocator.alloc<AstTypePackExplicit>(begin.location, AstTypeList{paramTypes, varargAnnotation});
        if (options.storeCstData)
            cstNodeMap[node] = allocator.alloc<CstTypePackExplicit>(parameterStart.location.begin, closeArgsLocation.begin, copy(argCommaPositions));
        return {{}, node};
    }

    AstArray<std::optional<AstArgumentName>> paramNames = copy(names);

    Position returnArrowPosition = lexer.current().location.begin;
    AstType* node = parseFunctionTypeTail(begin, attributes, generics, genericPacks, paramTypes, paramNames, varargAnnotation);
    if (options.storeCstData && node->is<AstTypeFunction>())
    {
        cstNodeMap[node] = allocator.alloc<CstTypeFunction>(
            genericsOpenPosition,
            genericsCommaPositions,
            genericsClosePosition,
            parameterStart.location.begin,
            copy(nameColonPositions),
            copy(argCommaPositions),
            closeArgsLocation.begin,
            returnArrowPosition
        );
    }
    return {node, {}};
}

AstType* Parser::parseFunctionTypeTail(
    const Lexeme& begin,
    const AstArray<AstAttr*>& attributes,
    AstArray<AstGenericType*> generics,
    AstArray<AstGenericTypePack*> genericPacks,
    AstArray<AstType*> params,
    AstArray<std::optional<AstArgumentName>> paramNames,
    AstTypePack* varargAnnotation
)
{
    incrementRecursionCounter("type annotation");

    if (lexer.current().type == ':')
    {
        report(lexer.current().location, "Return types in function type annotations are written after '->' instead of ':'");
        lexer.next();
    }
    // Users occasionally write '()' as the 'unit' type when they actually want to use 'nil', here we'll try to give a more specific error
    else if (lexer.current().type != Lexeme::SkinnyArrow && generics.size == 0 && genericPacks.size == 0 && params.size == 0)
    {
        report(Location(begin.location, lexer.previousLocation()), "Expected '->' after '()' when parsing function type; did you mean 'nil'?");

        return allocator.alloc<AstTypeReference>(begin.location, std::nullopt, nameNil, std::nullopt, begin.location);
    }
    else
    {
        expectAndConsume(Lexeme::SkinnyArrow, "function type");
    }

    auto returnType = parseReturnType();
    LUAU_ASSERT(returnType);

    AstTypeList paramTypes = AstTypeList{params, varargAnnotation};
    return allocator.alloc<AstTypeFunction>(
        Location(begin.location, returnType->location), attributes, generics, genericPacks, paramTypes, paramNames, returnType
    );
}

static bool isTypeFollow(Lexeme::Type c)
{
    return c == '|' || c == '?' || c == '&';
}

// Type ::=
//      nil |
//      Name[`.' Name] [`<' namelist `>'] |
//      `{' [PropList] `}' |
//      `(' [TypeList] `)' `->` ReturnType
//      `typeof` Type
AstType* Parser::parseTypeSuffix(AstType* type, const Location& begin)
{
    TempVector<AstType*> parts(scratchType);
    TempVector<Position> separatorPositions(scratchPosition);
    std::optional<Position> leadingPosition = std::nullopt;

    if (type != nullptr)
        parts.push_back(type);

    incrementRecursionCounter("type annotation");

    bool isUnion = false;
    bool isIntersection = false;
    unsigned int optionalCount = 0;

    Location location = begin;

    while (true)
    {
        Lexeme::Type c = lexer.current().type;
        Position separatorPosition = lexer.current().location.begin;
        if (c == '|')
        {
            nextLexeme();

            unsigned int oldRecursionCount = recursionCounter;
            parts.push_back(parseSimpleType(/* allowPack= */ false).type);
            recursionCounter = oldRecursionCount;

            isUnion = true;

            if (options.storeCstData)
            {
                if (type == nullptr && !leadingPosition.has_value())
                    leadingPosition = separatorPosition;
                else
                    separatorPositions.push_back(separatorPosition);
            }
        }
        else if (c == '?')
        {
            LUAU_ASSERT(parts.size() >= 1);

            Location loc = lexer.current().location;
            nextLexeme();

            parts.push_back(allocator.alloc<AstTypeOptional>(Location(loc)));
            optionalCount++;

            isUnion = true;
        }
        else if (c == '&')
        {
            nextLexeme();

            unsigned int oldRecursionCount = recursionCounter;
            parts.push_back(parseSimpleType(/* allowPack= */ false).type);
            recursionCounter = oldRecursionCount;

            isIntersection = true;

            if (options.storeCstData)
            {
                if (type == nullptr && !leadingPosition.has_value())
                    leadingPosition = separatorPosition;
                else
                    separatorPositions.push_back(separatorPosition);
            }
        }
        else if (c == Lexeme::Dot3)
        {
            report(lexer.current().location, "Unexpected '...' after type annotation");
            nextLexeme();
        }
        else
            break;

        if (parts.size() > unsigned(FInt::LuauTypeLengthLimit) + optionalCount)
            ParseError::raise(parts.back()->location, "Exceeded allowed type length; simplify your type annotation to make the code compile");
    }

    if (parts.size() == 1 && !isUnion && !isIntersection)
        return parts[0];
    if (isUnion && isIntersection)
    {
        return reportTypeError(
            Location(begin, parts.back()->location),
            copy(parts),
            "Mixing union and intersection types is not allowed; consider wrapping in parentheses."
        );
    }

    location.end = parts.back()->location.end;

    if (isUnion)
    {
        AstTypeUnion* node = allocator.alloc<AstTypeUnion>(location, copy(parts));
        if (options.storeCstData)
            cstNodeMap[node] = allocator.alloc<CstTypeUnion>(leadingPosition, copy(separatorPositions));
        return node;
    }

    if (isIntersection)
    {
        AstTypeIntersection* node = allocator.alloc<AstTypeIntersection>(location, copy(parts));
        if (options.storeCstData)
            cstNodeMap[node] = allocator.alloc<CstTypeIntersection>(leadingPosition, copy(separatorPositions));
        return node;
    }

    LUAU_ASSERT(false);
    ParseError::raise(begin, "Composite type was not an intersection or union.");
}

AstTypeOrPack Parser::parseSimpleTypeOrPack()
{
    unsigned int oldRecursionCount = recursionCounter;
    // recursion counter is incremented in parseSimpleType

    Location begin = lexer.current().location;

    auto [type, typePack] = parseSimpleType(/* allowPack= */ true);

    if (typePack)
    {
        LUAU_ASSERT(!type);
        return {{}, typePack};
    }

    recursionCounter = oldRecursionCount;

    return {parseTypeSuffix(type, begin), {}};
}

AstType* Parser::parseType(bool inDeclarationContext)
{
    unsigned int oldRecursionCount = recursionCounter;
    // recursion counter is incremented in parseSimpleType and/or parseTypeSuffix

    Location begin = lexer.current().location;

    AstType* type = nullptr;

    Lexeme::Type c = lexer.current().type;
    if (c != '|' && c != '&')
    {
        type = parseSimpleType(/* allowPack= */ false, /* in declaration context */ inDeclarationContext).type;
        recursionCounter = oldRecursionCount;
    }

    AstType* typeWithSuffix = parseTypeSuffix(type, begin);
    recursionCounter = oldRecursionCount;

    return typeWithSuffix;
}

// Type ::= nil | Name[`.' Name] [ `<' Type [`,' ...] `>' ] | `typeof' `(' expr `)' | `{' [PropList] `}'
//   | [`<' varlist `>'] `(' [TypeList] `)' `->` ReturnType
AstTypeOrPack Parser::parseSimpleType(bool allowPack, bool inDeclarationContext)
{
    incrementRecursionCounter("type annotation");

    Location start = lexer.current().location;

    AstArray<AstAttr*> attributes{nullptr, 0};

    if (lexer.current().type == Lexeme::Attribute)
    {
        if (!inDeclarationContext)
        {
            return {reportTypeError(start, {}, "attributes are not allowed in declaration context")};
        }
        else
        {
            attributes = Parser::parseAttributes();
            return parseFunctionType(allowPack, attributes);
        }
    }
    else if (lexer.current().type == Lexeme::ReservedNil)
    {
        nextLexeme();
        return {allocator.alloc<AstTypeReference>(start, std::nullopt, nameNil, std::nullopt, start), {}};
    }
    else if (lexer.current().type == Lexeme::ReservedTrue)
    {
        nextLexeme();
        return {allocator.alloc<AstTypeSingletonBool>(start, true)};
    }
    else if (lexer.current().type == Lexeme::ReservedFalse)
    {
        nextLexeme();
        return {allocator.alloc<AstTypeSingletonBool>(start, false)};
    }
    else if (lexer.current().type == Lexeme::RawString || lexer.current().type == Lexeme::QuotedString)
    {
        CstExprConstantString::QuoteStyle style;
        unsigned int blockDepth = 0;
        if (options.storeCstData)
            std::tie(style, blockDepth) = extractStringDetails();

        AstArray<char> originalString;
        if (std::optional<AstArray<char>> value = parseCharArray(options.storeCstData ? &originalString : nullptr))
        {
            AstArray<char> svalue = *value;
            auto node = allocator.alloc<AstTypeSingletonString>(start, svalue);
            if (options.storeCstData)
                cstNodeMap[node] = allocator.alloc<CstTypeSingletonString>(originalString, style, blockDepth);
            return {node};
        }
        else
            return {reportTypeError(start, {}, "String literal contains malformed escape sequence")};
    }
    else if (lexer.current().type == Lexeme::InterpStringBegin || lexer.current().type == Lexeme::InterpStringSimple)
    {
        parseInterpString();

        return {reportTypeError(start, {}, "Interpolated string literals cannot be used as types")};
    }
    else if (lexer.current().type == Lexeme::BrokenString)
    {
        nextLexeme();
        return {reportTypeError(start, {}, "Malformed string; did you forget to finish it?")};
    }
    else if (lexer.current().type == Lexeme::Name)
    {
        std::optional<AstName> prefix;
        std::optional<Position> prefixPointPosition;
        std::optional<Location> prefixLocation;
        Name name = parseName("type name");

        if (lexer.current().type == '.')
        {
            prefixPointPosition = lexer.current().location.begin;
            nextLexeme();

            prefix = name.name;
            prefixLocation = name.location;
            name = parseIndexName("field name", *prefixPointPosition);
        }
        else if (lexer.current().type == Lexeme::Dot3)
        {
            report(lexer.current().location, "Unexpected '...' after type name; type pack is not allowed in this context");
            nextLexeme();
        }
        else if (name.name == "typeof")
        {
            Lexeme typeofBegin = lexer.current();
            expectAndConsume('(', "typeof type");

            AstExpr* expr = parseExpr();

            Location end = lexer.current().location;

            expectMatchAndConsume(')', typeofBegin);

            AstTypeTypeof* node = allocator.alloc<AstTypeTypeof>(Location(start, end), expr);
            if (options.storeCstData)
                cstNodeMap[node] = allocator.alloc<CstTypeTypeof>(typeofBegin.location.begin, end.begin);
            return {node, {}};
        }

        bool hasParameters = false;
        AstArray<AstTypeOrPack> parameters{};
        Position parametersOpeningPosition{0, 0};
        TempVector<Position> parametersCommaPositions(scratchPosition);
        Position parametersClosingPosition{0, 0};

        if (lexer.current().type == '<')
        {
            hasParameters = true;
            if (options.storeCstData)
                parameters = parseTypeParams(&parametersOpeningPosition, &parametersCommaPositions, &parametersClosingPosition);
            else
                parameters = parseTypeParams();
        }

        Location end = lexer.previousLocation();

        AstTypeReference* node =
            allocator.alloc<AstTypeReference>(Location(start, end), prefix, name.name, prefixLocation, name.location, hasParameters, parameters);
        if (options.storeCstData)
            cstNodeMap[node] = allocator.alloc<CstTypeReference>(
                prefixPointPosition, parametersOpeningPosition, copy(parametersCommaPositions), parametersClosingPosition
            );
        return {node, {}};
    }
    else if (lexer.current().type == '{')
    {
        return {parseTableType(/* inDeclarationContext */ inDeclarationContext), {}};
    }
    else if (lexer.current().type == '(' || lexer.current().type == '<')
    {
        return parseFunctionType(allowPack, AstArray<AstAttr*>({nullptr, 0}));
    }
    else if (lexer.current().type == Lexeme::ReservedFunction)
    {
        nextLexeme();

        return {
            reportTypeError(
                start,
                {},
                "Using 'function' as a type annotation is not supported, consider replacing with a function type annotation e.g. '(...any) -> "
                "...any'"
            ),
            {}
        };
    }
    else
    {
        // For a missing type annotation, capture 'space' between last token and the next one
        Location astErrorlocation(lexer.previousLocation().end, start.begin);
        // The parse error includes the next lexeme to make it easier to display where the error is (e.g. in an IDE or a CLI error message).
        // Including the current lexeme also makes the parse error consistent with other parse errors returned by Luau.
        Location parseErrorLocation(lexer.previousLocation().end, start.end);
        return {reportMissingTypeError(parseErrorLocation, astErrorlocation, "Expected type, got %s", lexer.current().toString().c_str()), {}};
    }
}

AstTypePack* Parser::parseVariadicArgumentTypePack()
{
    // Generic: a...
    if (lexer.current().type == Lexeme::Name && lexer.lookahead().type == Lexeme::Dot3)
    {
        Name name = parseName("generic name");
        Location end = lexer.current().location;

        // This will not fail because of the lookahead guard.
        expectAndConsume(Lexeme::Dot3, "generic type pack annotation");
        AstTypePackGeneric* node = allocator.alloc<AstTypePackGeneric>(Location(name.location, end), name.name);
        if (options.storeCstData)
            cstNodeMap[node] = allocator.alloc<CstTypePackGeneric>(end.begin);
        return node;
    }
    // Variadic: T
    else
    {
        AstType* variadicAnnotation = parseType();
        return allocator.alloc<AstTypePackVariadic>(variadicAnnotation->location, variadicAnnotation);
    }
}

AstTypePack* Parser::parseTypePack()
{
    // Variadic: ...T
    if (lexer.current().type == Lexeme::Dot3)
    {
        Location start = lexer.current().location;
        nextLexeme();
        AstType* varargTy = parseType();
        return allocator.alloc<AstTypePackVariadic>(Location(start, varargTy->location), varargTy);
    }
    // Generic: a...
    else if (lexer.current().type == Lexeme::Name && lexer.lookahead().type == Lexeme::Dot3)
    {
        Name name = parseName("generic name");
        Location end = lexer.current().location;

        // This will not fail because of the lookahead guard.
        expectAndConsume(Lexeme::Dot3, "generic type pack annotation");
        AstTypePackGeneric* node = allocator.alloc<AstTypePackGeneric>(Location(name.location, end), name.name);
        if (options.storeCstData)
            cstNodeMap[node] = allocator.alloc<CstTypePackGeneric>(end.begin);
        return node;
    }

    // TODO: shouldParseTypePack can be removed and parseTypePack can be called unconditionally instead
    LUAU_ASSERT(!"parseTypePack can't be called if shouldParseTypePack() returned false");
    return nullptr;
}

std::optional<AstExprUnary::Op> Parser::parseUnaryOp(const Lexeme& l)
{
    if (l.type == Lexeme::ReservedNot)
        return AstExprUnary::Not;
    else if (l.type == '-')
        return AstExprUnary::Minus;
    else if (l.type == '#')
        return AstExprUnary::Len;
    else
        return std::nullopt;
}

std::optional<AstExprBinary::Op> Parser::parseBinaryOp(const Lexeme& l)
{
    if (l.type == '+')
        return AstExprBinary::Add;
    else if (l.type == '-')
        return AstExprBinary::Sub;
    else if (l.type == '*')
        return AstExprBinary::Mul;
    else if (l.type == '/')
        return AstExprBinary::Div;
    else if (l.type == Lexeme::FloorDiv)
        return AstExprBinary::FloorDiv;
    else if (l.type == '%')
        return AstExprBinary::Mod;
    else if (l.type == '^')
        return AstExprBinary::Pow;
    else if (l.type == Lexeme::Dot2)
        return AstExprBinary::Concat;
    else if (l.type == Lexeme::NotEqual)
        return AstExprBinary::CompareNe;
    else if (l.type == Lexeme::Equal)
        return AstExprBinary::CompareEq;
    else if (l.type == '<')
        return AstExprBinary::CompareLt;
    else if (l.type == Lexeme::LessEqual)
        return AstExprBinary::CompareLe;
    else if (l.type == '>')
        return AstExprBinary::CompareGt;
    else if (l.type == Lexeme::GreaterEqual)
        return AstExprBinary::CompareGe;
    else if (l.type == Lexeme::ReservedAnd)
        return AstExprBinary::And;
    else if (l.type == Lexeme::ReservedOr)
        return AstExprBinary::Or;
    else
        return std::nullopt;
}

std::optional<AstExprBinary::Op> Parser::parseCompoundOp(const Lexeme& l)
{
    if (l.type == Lexeme::AddAssign)
        return AstExprBinary::Add;
    else if (l.type == Lexeme::SubAssign)
        return AstExprBinary::Sub;
    else if (l.type == Lexeme::MulAssign)
        return AstExprBinary::Mul;
    else if (l.type == Lexeme::DivAssign)
        return AstExprBinary::Div;
    else if (l.type == Lexeme::FloorDivAssign)
        return AstExprBinary::FloorDiv;
    else if (l.type == Lexeme::ModAssign)
        return AstExprBinary::Mod;
    else if (l.type == Lexeme::PowAssign)
        return AstExprBinary::Pow;
    else if (l.type == Lexeme::ConcatAssign)
        return AstExprBinary::Concat;
    else
        return std::nullopt;
}

std::optional<AstExprUnary::Op> Parser::checkUnaryConfusables()
{
    const Lexeme& curr = lexer.current();

    // early-out: need to check if this is a possible confusable quickly
    if (curr.type != '!')
        return {};

    // slow path: possible confusable
    Location start = curr.location;

    if (curr.type == '!')
    {
        report(start, "Unexpected '!'; did you mean 'not'?");
        return AstExprUnary::Not;
    }

    return {};
}

std::optional<AstExprBinary::Op> Parser::checkBinaryConfusables(const BinaryOpPriority binaryPriority[], unsigned int limit)
{
    const Lexeme& curr = lexer.current();

    // early-out: need to check if this is a possible confusable quickly
    if (curr.type != '&' && curr.type != '|' && curr.type != '!')
        return {};

    // slow path: possible confusable
    Location start = curr.location;
    Lexeme next = lexer.lookahead();

    if (curr.type == '&' && next.type == '&' && curr.location.end == next.location.begin && binaryPriority[AstExprBinary::And].left > limit)
    {
        nextLexeme();
        report(Location(start, next.location), "Unexpected '&&'; did you mean 'and'?");
        return AstExprBinary::And;
    }
    else if (curr.type == '|' && next.type == '|' && curr.location.end == next.location.begin && binaryPriority[AstExprBinary::Or].left > limit)
    {
        nextLexeme();
        report(Location(start, next.location), "Unexpected '||'; did you mean 'or'?");
        return AstExprBinary::Or;
    }
    else if (curr.type == '!' && next.type == '=' && curr.location.end == next.location.begin &&
             binaryPriority[AstExprBinary::CompareNe].left > limit)
    {
        nextLexeme();
        report(Location(start, next.location), "Unexpected '!='; did you mean '~='?");
        return AstExprBinary::CompareNe;
    }

    return std::nullopt;
}

// subexpr -> (asexp | unop subexpr) { binop subexpr }
// where `binop' is any binary operator with a priority higher than `limit'
AstExpr* Parser::parseExpr(unsigned int limit)
{
    static const BinaryOpPriority binaryPriority[] = {
        {6, 6},  // '+'
        {6, 6},  // '-'
        {7, 7},  // '*'
        {7, 7},  // '/'
        {7, 7},  // '//'
        {7, 7},  // `%'
        {10, 9}, // power (right associative)
        {5, 4},  // concat (right associative)
        {3, 3},  // inequality
        {3, 3},  // equality
        {3, 3},  // '<'
        {3, 3},  // '<='
        {3, 3},  // '>'
        {3, 3},  // '>='
        {2, 2},  // logical and
        {1, 1}   // logical or
    };

    static_assert(sizeof(binaryPriority) / sizeof(binaryPriority[0]) == size_t(AstExprBinary::Op__Count), "binaryPriority needs an entry per op");

    unsigned int oldRecursionCount = recursionCounter;

    // this handles recursive calls to parseSubExpr/parseExpr
    incrementRecursionCounter("expression");

    const unsigned int unaryPriority = 8;

    Location start = lexer.current().location;

    AstExpr* expr;

    std::optional<AstExprUnary::Op> uop = parseUnaryOp(lexer.current());

    if (!uop)
        uop = checkUnaryConfusables();

    if (uop)
    {
        Position opPosition = lexer.current().location.begin;
        nextLexeme();

        AstExpr* subexpr = parseExpr(unaryPriority);

        expr = allocator.alloc<AstExprUnary>(Location(start, subexpr->location), *uop, subexpr);
        if (options.storeCstData)
            cstNodeMap[expr] = allocator.alloc<CstExprOp>(opPosition);
    }
    else
    {
        expr = parseAssertionExpr();
    }

    // expand while operators have priorities higher than `limit'
    std::optional<AstExprBinary::Op> op = parseBinaryOp(lexer.current());

    if (!op)
        op = checkBinaryConfusables(binaryPriority, limit);

    while (op && binaryPriority[*op].left > limit)
    {
        Position opPosition = lexer.current().location.begin;
        nextLexeme();

        // read sub-expression with higher priority
        AstExpr* next = parseExpr(binaryPriority[*op].right);

        expr = allocator.alloc<AstExprBinary>(Location(start, next->location), *op, expr, next);
        if (options.storeCstData)
            cstNodeMap[expr] = allocator.alloc<CstExprOp>(opPosition);
        op = parseBinaryOp(lexer.current());

        if (!op)
            op = checkBinaryConfusables(binaryPriority, limit);

        // note: while the parser isn't recursive here, we're generating recursive structures of unbounded depth
        incrementRecursionCounter("expression");
    }

    recursionCounter = oldRecursionCount;

    return expr;
}

// NAME
AstExpr* Parser::parseNameExpr(const char* context)
{
    std::optional<Name> name = parseNameOpt(context);

    if (!name)
        return allocator.alloc<AstExprError>(lexer.current().location, copy<AstExpr*>({}), unsigned(parseErrors.size() - 1));

    AstLocal* const* value = localMap.find(name->name);

    if (value && *value)
    {
        AstLocal* local = *value;

        if (local->functionDepth < typeFunctionDepth)
            return reportExprError(lexer.current().location, {}, "Type function cannot reference outer local '%s'", local->name.value);

        return allocator.alloc<AstExprLocal>(name->location, local, local->functionDepth != functionStack.size() - 1);
    }

    return allocator.alloc<AstExprGlobal>(name->location, name->name);
}

// prefixexp -> NAME | '(' expr ')'
AstExpr* Parser::parsePrefixExpr()
{
    if (lexer.current().type == '(')
    {
        Position start = lexer.current().location.begin;

        MatchLexeme matchParen = lexer.current();
        nextLexeme();

        AstExpr* expr = parseExpr();

        Position end = lexer.current().location.end;

        if (lexer.current().type != ')')
        {
            const char* suggestion = (lexer.current().type == '=') ? "; did you mean to use '{' when defining a table?" : nullptr;

            expectMatchAndConsumeFail(static_cast<Lexeme::Type>(')'), matchParen, suggestion);

            end = lexer.previousLocation().end;
        }
        else
        {
            nextLexeme();
        }

        return allocator.alloc<AstExprGroup>(Location(start, end), expr);
    }
    else
    {
        return parseNameExpr("expression");
    }
}

// primaryexp -> prefixexp { `.' NAME | `[' exp `]' | `:' NAME funcargs | funcargs }
AstExpr* Parser::parsePrimaryExpr(bool asStatement)
{
    Position start = lexer.current().location.begin;

    AstExpr* expr = parsePrefixExpr();

    unsigned int oldRecursionCount = recursionCounter;

    while (true)
    {
        if (lexer.current().type == '.')
        {
            Position opPosition = lexer.current().location.begin;
            nextLexeme();

            Name index = parseIndexName(nullptr, opPosition);

            expr = allocator.alloc<AstExprIndexName>(Location(start, index.location.end), expr, index.name, index.location, opPosition, '.');
        }
        else if (lexer.current().type == '[')
        {
            MatchLexeme matchBracket = lexer.current();
            nextLexeme();

            AstExpr* index = parseExpr();

            Position closeBracketPosition = lexer.current().location.begin;
            Position end = lexer.current().location.end;

            expectMatchAndConsume(']', matchBracket);

            expr = allocator.alloc<AstExprIndexExpr>(Location(start, end), expr, index);
            if (options.storeCstData)
                cstNodeMap[expr] = allocator.alloc<CstExprIndexExpr>(matchBracket.position, closeBracketPosition);
        }
        else if (lexer.current().type == ':')
        {
            Position opPosition = lexer.current().location.begin;
            nextLexeme();

            Name index = parseIndexName("method name", opPosition);
            AstExpr* func = allocator.alloc<AstExprIndexName>(Location(start, index.location.end), expr, index.name, index.location, opPosition, ':');

            expr = parseFunctionArgs(func, true);
        }
        else if (lexer.current().type == '(')
        {
            // This error is handled inside 'parseFunctionArgs' as well, but for better error recovery we need to break out the current loop here
            if (!asStatement && expr->location.end.line != lexer.current().location.begin.line)
            {
                reportAmbiguousCallError();
                break;
            }

            expr = parseFunctionArgs(expr, false);
        }
        else if (lexer.current().type == '{' || lexer.current().type == Lexeme::RawString || lexer.current().type == Lexeme::QuotedString)
        {
            expr = parseFunctionArgs(expr, false);
        }
        else
        {
            break;
        }

        // note: while the parser isn't recursive here, we're generating recursive structures of unbounded depth
        incrementRecursionCounter("expression");
    }

    recursionCounter = oldRecursionCount;

    return expr;
}

// asexp -> simpleexp [`::' Type]
AstExpr* Parser::parseAssertionExpr()
{
    Location start = lexer.current().location;
    AstExpr* expr = parseSimpleExpr();

    if (lexer.current().type == Lexeme::DoubleColon)
    {
        CstExprTypeAssertion* cstNode = nullptr;
        if (options.storeCstData)
        {
            Position opPosition = lexer.current().location.begin;
            cstNode = allocator.alloc<CstExprTypeAssertion>(opPosition);
        }
        nextLexeme();
        AstType* annotation = parseType();
        AstExprTypeAssertion* node = allocator.alloc<AstExprTypeAssertion>(Location(start, annotation->location), expr, annotation);
        if (options.storeCstData)
            cstNodeMap[node] = cstNode;
        return node;
    }
    else
        return expr;
}

static ConstantNumberParseResult parseInteger(double& result, const char* data, int base)
{
    LUAU_ASSERT(base == 2 || base == 16);

    char* end = nullptr;
    unsigned long long value = strtoull(data, &end, base);

    if (*end != 0)
        return ConstantNumberParseResult::Malformed;

    result = double(value);

    if (value == ULLONG_MAX && errno == ERANGE)
    {
        // 'errno' might have been set before we called 'strtoull', but we don't want the overhead of resetting a TLS variable on each call
        // so we only reset it when we get a result that might be an out-of-range error and parse again to make sure
        errno = 0;
        value = strtoull(data, &end, base);

        if (errno == ERANGE)
            return base == 2 ? ConstantNumberParseResult::BinOverflow : ConstantNumberParseResult::HexOverflow;
    }

    if (value >= (1ull << 53) && static_cast<unsigned long long>(result) != value)
        return ConstantNumberParseResult::Imprecise;

    return ConstantNumberParseResult::Ok;
}

static ConstantNumberParseResult parseDouble(double& result, const char* data)
{
    // binary literal
    if (data[0] == '0' && (data[1] == 'b' || data[1] == 'B') && data[2])
        return parseInteger(result, data + 2, 2);

    // hexadecimal literal
    if (data[0] == '0' && (data[1] == 'x' || data[1] == 'X') && data[2])
        return parseInteger(result, data, 16); // pass in '0x' prefix, it's handled by 'strtoull'

    char* end = nullptr;
    double value = strtod(data, &end);

    // trailing non-numeric characters
    if (*end != 0)
        return ConstantNumberParseResult::Malformed;

    result = value;

    // for linting, we detect integer constants that are parsed imprecisely
    // since the check is expensive we only perform it when the number is larger than the precise integer range
    if (value >= double(1ull << 53) && strspn(data, "0123456789") == strlen(data))
    {
        char repr[512];
        snprintf(repr, sizeof(repr), "%.0f", value);

        if (strcmp(repr, data) != 0)
            return ConstantNumberParseResult::Imprecise;
    }

    return ConstantNumberParseResult::Ok;
}

// simpleexp -> NUMBER | STRING | NIL | true | false | ... | constructor | [attributes] FUNCTION body | primaryexp
AstExpr* Parser::parseSimpleExpr()
{
    Location start = lexer.current().location;

    AstArray<AstAttr*> attributes{nullptr, 0};

    if (lexer.current().type == Lexeme::Attribute)
    {
        attributes = parseAttributes();

        if (lexer.current().type != Lexeme::ReservedFunction)
        {
            return reportExprError(
                start, {}, "Expected 'function' declaration after attribute, but got %s instead", lexer.current().toString().c_str()
            );
        }
    }

    if (lexer.current().type == Lexeme::ReservedNil)
    {
        nextLexeme();

        return allocator.alloc<AstExprConstantNil>(start);
    }
    else if (lexer.current().type == Lexeme::ReservedTrue)
    {
        nextLexeme();

        return allocator.alloc<AstExprConstantBool>(start, true);
    }
    else if (lexer.current().type == Lexeme::ReservedFalse)
    {
        nextLexeme();

        return allocator.alloc<AstExprConstantBool>(start, false);
    }
    else if (lexer.current().type == Lexeme::ReservedFunction)
    {
        Lexeme matchFunction = lexer.current();
        nextLexeme();

        return parseFunctionBody(false, matchFunction, AstName(), nullptr, attributes).first;
    }
    else if (lexer.current().type == Lexeme::Number)
    {
        return parseNumber();
    }
    else if (lexer.current().type == Lexeme::RawString || lexer.current().type == Lexeme::QuotedString ||
             lexer.current().type == Lexeme::InterpStringSimple)
    {
        return parseString();
    }
    else if (lexer.current().type == Lexeme::InterpStringBegin)
    {
        return parseInterpString();
    }
    else if (lexer.current().type == Lexeme::BrokenString)
    {
        nextLexeme();
        return reportExprError(start, {}, "Malformed string; did you forget to finish it?");
    }
    else if (lexer.current().type == Lexeme::BrokenInterpDoubleBrace)
    {
        nextLexeme();
        return reportExprError(start, {}, "Double braces are not permitted within interpolated strings; did you mean '\\{'?");
    }
    else if (lexer.current().type == Lexeme::Dot3)
    {
        if (functionStack.back().vararg)
        {
            nextLexeme();

            return allocator.alloc<AstExprVarargs>(start);
        }
        else
        {
            nextLexeme();

            return reportExprError(start, {}, "Cannot use '...' outside of a vararg function");
        }
    }
    else if (lexer.current().type == '{')
    {
        return parseTableConstructor();
    }
    else if (lexer.current().type == Lexeme::ReservedIf)
    {
        return parseIfElseExpr();
    }
    else
    {
        return parsePrimaryExpr(/* asStatement= */ false);
    }
}

// args ::=  `(' [explist] `)' | tableconstructor | String
AstExpr* Parser::parseFunctionArgs(AstExpr* func, bool self)
{
    if (lexer.current().type == '(')
    {
        Position argStart = lexer.current().location.end;
        if (func->location.end.line != lexer.current().location.begin.line)
            reportAmbiguousCallError();

        MatchLexeme matchParen = lexer.current();
        nextLexeme();

        TempVector<AstExpr*> args(scratchExpr);
        TempVector<Position> commaPositions(scratchPosition);

        if (lexer.current().type != ')')
            parseExprList(args, options.storeCstData ? &commaPositions : nullptr);

        Location end = lexer.current().location;
        Position argEnd = end.end;

        expectMatchAndConsume(')', matchParen);

        AstExprCall* node = allocator.alloc<AstExprCall>(Location(func->location, end), func, copy(args), self, Location(argStart, argEnd));
        if (options.storeCstData)
            cstNodeMap[node] = allocator.alloc<CstExprCall>(matchParen.position, lexer.previousLocation().begin, copy(commaPositions));
        return node;
    }
    else if (lexer.current().type == '{')
    {
        Position argStart = lexer.current().location.end;
        AstExpr* expr = parseTableConstructor();
        Position argEnd = lexer.previousLocation().end;

        AstExprCall* node =
            allocator.alloc<AstExprCall>(Location(func->location, expr->location), func, copy(&expr, 1), self, Location(argStart, argEnd));
        if (options.storeCstData)
            cstNodeMap[node] = allocator.alloc<CstExprCall>(std::nullopt, std::nullopt, AstArray<Position>{nullptr, 0});
        return node;
    }
    else if (lexer.current().type == Lexeme::RawString || lexer.current().type == Lexeme::QuotedString)
    {
        Location argLocation = lexer.current().location;
        AstExpr* expr = parseString();

        AstExprCall* node = allocator.alloc<AstExprCall>(Location(func->location, expr->location), func, copy(&expr, 1), self, argLocation);
        if (options.storeCstData)
            cstNodeMap[node] = allocator.alloc<CstExprCall>(std::nullopt, std::nullopt, AstArray<Position>{nullptr, 0});
        return node;
    }
    else
    {
        return reportFunctionArgsError(func, self);
    }
}

LUAU_NOINLINE AstExpr* Parser::reportFunctionArgsError(AstExpr* func, bool self)
{
    if (self && lexer.current().location.begin.line != func->location.end.line)
    {
        return reportExprError(func->location, copy({func}), "Expected function call arguments after '('");
    }
    else
    {
        return reportExprError(
            Location(func->location.begin, lexer.current().location.begin),
            copy({func}),
            "Expected '(', '{' or <string> when parsing function call, got %s",
            lexer.current().toString().c_str()
        );
    }
}

LUAU_NOINLINE void Parser::reportAmbiguousCallError()
{
    report(
        lexer.current().location,
        "Ambiguous syntax: this looks like an argument list for a function call, but could also be a start of "
        "new statement; use ';' to separate statements"
    );
}

std::optional<CstExprTable::Separator> Parser::tableSeparator()
{
    if (lexer.current().type == ',')
        return CstExprTable::Comma;
    else if (lexer.current().type == ';')
        return CstExprTable::Semicolon;
    else
        return std::nullopt;
}

// tableconstructor ::= `{' [fieldlist] `}'
// fieldlist ::= field {fieldsep field} [fieldsep]
// field ::= `[' exp `]' `=' exp | Name `=' exp | exp
// fieldsep ::= `,' | `;'
AstExpr* Parser::parseTableConstructor()
{
    TempVector<AstExprTable::Item> items(scratchItem);
    TempVector<CstExprTable::Item> cstItems(scratchCstItem);

    Location start = lexer.current().location;

    MatchLexeme matchBrace = lexer.current();
    expectAndConsume('{', "table literal");
    unsigned lastElementIndent = 0;

    while (lexer.current().type != '}')
    {
        lastElementIndent = lexer.current().location.begin.column;

        if (lexer.current().type == '[')
        {
            Position indexerOpenPosition = lexer.current().location.begin;
            MatchLexeme matchLocationBracket = lexer.current();
            nextLexeme();

            AstExpr* key = parseExpr();

            Position indexerClosePosition = lexer.current().location.begin;
            expectMatchAndConsume(']', matchLocationBracket);

            Position equalsPosition = lexer.current().location.begin;
            expectAndConsume('=', "table field");

            AstExpr* value = parseExpr();

            items.push_back({AstExprTable::Item::General, key, value});
            if (options.storeCstData)
                cstItems.push_back({indexerOpenPosition, indexerClosePosition, equalsPosition, tableSeparator(), lexer.current().location.begin});
        }
        else if (lexer.current().type == Lexeme::Name && lexer.lookahead().type == '=')
        {
            Name name = parseName("table field");

            Position equalsPosition = lexer.current().location.begin;
            expectAndConsume('=', "table field");

            AstArray<char> nameString;
            nameString.data = const_cast<char*>(name.name.value);
            nameString.size = strlen(name.name.value);

            AstExpr* key = allocator.alloc<AstExprConstantString>(name.location, nameString, AstExprConstantString::Unquoted);
            AstExpr* value = parseExpr();

            if (AstExprFunction* func = value->as<AstExprFunction>())
                func->debugname = name.name;

            items.push_back({AstExprTable::Item::Record, key, value});
            if (options.storeCstData)
                cstItems.push_back({std::nullopt, std::nullopt, equalsPosition, tableSeparator(), lexer.current().location.begin});
        }
        else
        {
            AstExpr* expr = parseExpr();

            items.push_back({AstExprTable::Item::List, nullptr, expr});
            if (options.storeCstData)
                cstItems.push_back({std::nullopt, std::nullopt, std::nullopt, tableSeparator(), lexer.current().location.begin});
        }

        if (lexer.current().type == ',' || lexer.current().type == ';')
        {
            nextLexeme();
        }
        else if ((lexer.current().type == '[' || lexer.current().type == Lexeme::Name) && lexer.current().location.begin.column == lastElementIndent)
        {
            report(lexer.current().location, "Expected ',' after table constructor element");
        }
        else if (lexer.current().type != '}')
        {
            break;
        }
    }

    Location end = lexer.current().location;

    if (!expectMatchAndConsume('}', matchBrace))
        end = lexer.previousLocation();

    AstExprTable* node = allocator.alloc<AstExprTable>(Location(start, end), copy(items));
    if (options.storeCstData)
        cstNodeMap[node] = allocator.alloc<CstExprTable>(copy(cstItems));
    return node;
}

AstExpr* Parser::parseIfElseExpr()
{
    bool hasElse = false;
    Location start = lexer.current().location;

    nextLexeme(); // skip if / elseif

    AstExpr* condition = parseExpr();

    Position thenPosition = lexer.current().location.begin;
    bool hasThen = expectAndConsume(Lexeme::ReservedThen, "if then else expression");

    AstExpr* trueExpr = parseExpr();
    AstExpr* falseExpr = nullptr;

    Position elsePosition = lexer.current().location.begin;
    bool isElseIf = false;
    if (lexer.current().type == Lexeme::ReservedElseif)
    {
        unsigned int oldRecursionCount = recursionCounter;
        incrementRecursionCounter("expression");
        hasElse = true;
        falseExpr = parseIfElseExpr();
        recursionCounter = oldRecursionCount;
        isElseIf = true;
    }
    else
    {
        hasElse = expectAndConsume(Lexeme::ReservedElse, "if then else expression");
        falseExpr = parseExpr();
    }

    Location end = falseExpr->location;

    AstExprIfElse* node = allocator.alloc<AstExprIfElse>(Location(start, end), condition, hasThen, trueExpr, hasElse, falseExpr);
    if (options.storeCstData)
        cstNodeMap[node] = allocator.alloc<CstExprIfElse>(thenPosition, elsePosition, isElseIf);
    return node;
}

// Name
std::optional<Parser::Name> Parser::parseNameOpt(const char* context)
{
    if (lexer.current().type != Lexeme::Name)
    {
        reportNameError(context);

        return {};
    }

    Name result(AstName(lexer.current().name), lexer.current().location);

    nextLexeme();

    return result;
}

Parser::Name Parser::parseName(const char* context)
{
    if (std::optional<Name> name = parseNameOpt(context))
        return *name;

    Location location = lexer.current().location;
    location.end = location.begin;

    return Name(nameError, location);
}

Parser::Name Parser::parseIndexName(const char* context, const Position& previous)
{
    if (std::optional<Name> name = parseNameOpt(context))
        return *name;

    // If we have a reserved keyword next at the same line, assume it's an incomplete name
    if (lexer.current().type >= Lexeme::Reserved_BEGIN && lexer.current().type < Lexeme::Reserved_END &&
        lexer.current().location.begin.line == previous.line)
    {
        Name result(AstName(lexer.current().name), lexer.current().location);

        nextLexeme();

        return result;
    }

    Location location = lexer.current().location;
    location.end = location.begin;

    return Name(nameError, location);
}

std::pair<AstArray<AstGenericType*>, AstArray<AstGenericTypePack*>> Parser::parseGenericTypeList(
    bool withDefaultValues,
    Position* openPosition,
    AstArray<Position>* commaPositions,
    Position* closePosition
)
{
    TempVector<AstGenericType*> names{scratchGenericTypes};
    TempVector<AstGenericTypePack*> namePacks{scratchGenericTypePacks};
    TempVector<Position> localCommaPositions{scratchPosition};

    if (lexer.current().type == '<')
    {
        Lexeme begin = lexer.current();
        if (openPosition)
            *openPosition = begin.location.begin;
        nextLexeme();

        bool seenPack = false;
        bool seenDefault = false;

        while (true)
        {
            Location nameLocation = lexer.current().location;
            AstName name = parseName().name;
            if (lexer.current().type == Lexeme::Dot3 || seenPack)
            {
                seenPack = true;

                Position ellipsisPosition = lexer.current().location.begin;
                if (lexer.current().type != Lexeme::Dot3)
                    report(lexer.current().location, "Generic types come before generic type packs");
                else
                    nextLexeme();

                if (withDefaultValues && lexer.current().type == '=')
                {
                    seenDefault = true;
                    Position equalsPosition = lexer.current().location.begin;
                    nextLexeme();

                    if (shouldParseTypePack(lexer))
                    {
                        AstTypePack* typePack = parseTypePack();

                        AstGenericTypePack* node = allocator.alloc<AstGenericTypePack>(nameLocation, name, typePack);
                        if (options.storeCstData)
                            cstNodeMap[node] = allocator.alloc<CstGenericTypePack>(ellipsisPosition, equalsPosition);
                        namePacks.push_back(node);
                    }
                    else
                    {
                        auto [type, typePack] = parseSimpleTypeOrPack();

                        if (type)
                            report(type->location, "Expected type pack after '=', got type");

                        AstGenericTypePack* node = allocator.alloc<AstGenericTypePack>(nameLocation, name, typePack);
                        if (options.storeCstData)
                            cstNodeMap[node] = allocator.alloc<CstGenericTypePack>(ellipsisPosition, equalsPosition);
                        namePacks.push_back(node);
                    }
                }
                else
                {
                    if (seenDefault)
                        report(lexer.current().location, "Expected default type pack after type pack name");

                    AstGenericTypePack* node = allocator.alloc<AstGenericTypePack>(nameLocation, name, nullptr);
                    if (options.storeCstData)
                        cstNodeMap[node] = allocator.alloc<CstGenericTypePack>(ellipsisPosition, std::nullopt);
                    namePacks.push_back(node);
                }
            }
            else
            {
                if (withDefaultValues && lexer.current().type == '=')
                {
                    seenDefault = true;
                    Position equalsPosition = lexer.current().location.begin;
                    nextLexeme();

                    AstType* defaultType = parseType();

                    AstGenericType* node = allocator.alloc<AstGenericType>(nameLocation, name, defaultType);
                    if (options.storeCstData)
                        cstNodeMap[node] = allocator.alloc<CstGenericType>(equalsPosition);
                    names.push_back(node);
                }
                else
                {
                    if (seenDefault)
                        report(lexer.current().location, "Expected default type after type name");

                    AstGenericType* node = allocator.alloc<AstGenericType>(nameLocation, name, nullptr);
                    if (options.storeCstData)
                        cstNodeMap[node] = allocator.alloc<CstGenericType>(std::nullopt);
                    names.push_back(node);
                }
            }

            if (lexer.current().type == ',')
            {
                if (commaPositions)
                    localCommaPositions.push_back(lexer.current().location.begin);
                nextLexeme();

                if (lexer.current().type == '>')
                {
                    report(lexer.current().location, "Expected type after ',' but got '>' instead");
                    break;
                }
            }
            else
                break;
        }

        if (closePosition)
            *closePosition = lexer.current().location.begin;
        expectMatchAndConsume('>', begin);
    }

    if (commaPositions)
        *commaPositions = copy(localCommaPositions);

    AstArray<AstGenericType*> generics = copy(names);
    AstArray<AstGenericTypePack*> genericPacks = copy(namePacks);
    return {generics, genericPacks};
}

AstArray<AstTypeOrPack> Parser::parseTypeParams(Position* openingPosition, TempVector<Position>* commaPositions, Position* closingPosition)
{
    TempVector<AstTypeOrPack> parameters{scratchTypeOrPack};

    if (lexer.current().type == '<')
    {
        Lexeme begin = lexer.current();
        if (openingPosition)
            *openingPosition = begin.location.begin;
        nextLexeme();

        while (true)
        {
            if (shouldParseTypePack(lexer))
            {
                AstTypePack* typePack = parseTypePack();
                parameters.push_back({{}, typePack});
            }
            else if (lexer.current().type == '(')
            {
                Location begin = lexer.current().location;
                AstType* type = nullptr;
                AstTypePack* typePack = nullptr;
                Lexeme::Type c = lexer.current().type;

                if (c != '|' && c != '&')
                {
                    auto typeOrTypePack = parseSimpleType(/* allowPack */ true, /* inDeclarationContext */ false);
                    type = typeOrTypePack.type;
                    typePack = typeOrTypePack.typePack;
                }

                // Consider the following type:
                //
                //  X<(T)>
                //
                // Is this a type pack or a parenthesized type? The
                // assumption will be a type pack, as that's what allows one
                // to express either a singular type pack or a potential
                // complex type.

                if (typePack)
                {
                    auto explicitTypePack = typePack->as<AstTypePackExplicit>();
                    if (explicitTypePack && explicitTypePack->typeList.tailType == nullptr && explicitTypePack->typeList.types.size == 1 &&
                        isTypeFollow(lexer.current().type))
                    {
                        // If we parsed an explicit type pack with a single
                        // type in it (something of the form `(T)`), and
                        // the next lexeme is one that follows a type
                        // (&, |, ?), then assume that this was actually a
                        // parenthesized type.
                        auto parenthesizedType = explicitTypePack->typeList.types.data[0];
                        parameters.push_back(
                            {parseTypeSuffix(allocator.alloc<AstTypeGroup>(parenthesizedType->location, parenthesizedType), begin), {}}
                        );
                    }
                    else
                    {
                        // Otherwise, it's a type pack.
                        parameters.push_back({{}, typePack});
                    }
                }
                else
                {
                    // There's two cases in which `typePack` will be null:
                    // - We try to parse a simple type or a type pack, and
                    //   we get a simple type: there's no ambiguity and
                    //   we attempt to parse a complex type.
                    // - The next lexeme was a `|` or `&` indicating a
                    //   union or intersection type with a leading
                    //   separator. We just fall right into
                    //   `parseTypeSuffix`, which allows its first
                    //   argument to be `nullptr`
                    parameters.push_back({parseTypeSuffix(type, begin), {}});
                }
            }
            else if (lexer.current().type == '>' && parameters.empty())
            {
                break;
            }
            else
            {
                parameters.push_back({parseType(), {}});
            }

            if (lexer.current().type == ',')
            {
                if (commaPositions)
                    commaPositions->push_back(lexer.current().location.begin);
                nextLexeme();
            }
            else
                break;
        }

        if (closingPosition)
            *closingPosition = lexer.current().location.begin;
        expectMatchAndConsume('>', begin);
    }

    return copy(parameters);
}

std::optional<AstArray<char>> Parser::parseCharArray(AstArray<char>* originalString)
{
    LUAU_ASSERT(
        lexer.current().type == Lexeme::QuotedString || lexer.current().type == Lexeme::RawString ||
        lexer.current().type == Lexeme::InterpStringSimple
    );

    scratchData.assign(lexer.current().data, lexer.current().getLength());
    if (originalString)
        *originalString = copy(scratchData);

    if (lexer.current().type == Lexeme::QuotedString || lexer.current().type == Lexeme::InterpStringSimple)
    {
        if (!Lexer::fixupQuotedString(scratchData))
        {
            nextLexeme();
            return std::nullopt;
        }
    }
    else
    {
        Lexer::fixupMultilineString(scratchData);
    }

    AstArray<char> value = copy(scratchData);
    nextLexeme();
    return value;
}

AstExpr* Parser::parseString()
{
    Location location = lexer.current().location;

    AstExprConstantString::QuoteStyle style;
    switch (lexer.current().type)
    {
    case Lexeme::QuotedString:
    case Lexeme::InterpStringSimple:
        style = AstExprConstantString::QuotedSimple;
        break;
    case Lexeme::RawString:
        style = AstExprConstantString::QuotedRaw;
        break;
    default:
        LUAU_ASSERT(false && "Invalid string type");
    }

    CstExprConstantString::QuoteStyle fullStyle;
    unsigned int blockDepth;
    if (options.storeCstData)
        std::tie(fullStyle, blockDepth) = extractStringDetails();

    AstArray<char> originalString;
    if (std::optional<AstArray<char>> value = parseCharArray(options.storeCstData ? &originalString : nullptr))
    {
        AstExprConstantString* node = allocator.alloc<AstExprConstantString>(location, *value, style);
        if (options.storeCstData)
            cstNodeMap[node] = allocator.alloc<CstExprConstantString>(originalString, fullStyle, blockDepth);
        return node;
    }
    else
        return reportExprError(location, {}, "String literal contains malformed escape sequence");
}

AstExpr* Parser::parseInterpString()
{
    TempVector<AstArray<char>> strings(scratchString);
    TempVector<AstArray<char>> sourceStrings(scratchString2);
    TempVector<Position> stringPositions(scratchPosition);
    TempVector<AstExpr*> expressions(scratchExpr);

    Location startLocation = lexer.current().location;
    Location endLocation;

    do
    {
        Lexeme currentLexeme = lexer.current();
        LUAU_ASSERT(
            currentLexeme.type == Lexeme::InterpStringBegin || currentLexeme.type == Lexeme::InterpStringMid ||
            currentLexeme.type == Lexeme::InterpStringEnd || currentLexeme.type == Lexeme::InterpStringSimple
        );

        endLocation = currentLexeme.location;

        scratchData.assign(currentLexeme.data, currentLexeme.getLength());

        if (options.storeCstData)
        {
            sourceStrings.push_back(copy(scratchData));
            stringPositions.push_back(currentLexeme.location.begin);
        }

        if (!Lexer::fixupQuotedString(scratchData))
        {
            nextLexeme();
            return reportExprError(Location{startLocation, endLocation}, {}, "Interpolated string literal contains malformed escape sequence");
        }

        AstArray<char> chars = copy(scratchData);

        nextLexeme();

        strings.push_back(chars);

        if (currentLexeme.type == Lexeme::InterpStringEnd || currentLexeme.type == Lexeme::InterpStringSimple)
        {
            break;
        }

        bool errorWhileChecking = false;

        switch (lexer.current().type)
        {
        case Lexeme::InterpStringMid:
        case Lexeme::InterpStringEnd:
        {
            errorWhileChecking = true;
            nextLexeme();
            expressions.push_back(reportExprError(endLocation, {}, "Malformed interpolated string, expected expression inside '{}'"));
            break;
        }
        case Lexeme::BrokenString:
        {
            errorWhileChecking = true;
            nextLexeme();
            expressions.push_back(reportExprError(endLocation, {}, "Malformed interpolated string; did you forget to add a '`'?"));
            break;
        }
        default:
            expressions.push_back(parseExpr());
        }

        if (errorWhileChecking)
        {
            break;
        }

        switch (lexer.current().type)
        {
        case Lexeme::InterpStringBegin:
        case Lexeme::InterpStringMid:
        case Lexeme::InterpStringEnd:
            break;
        case Lexeme::BrokenInterpDoubleBrace:
            nextLexeme();
            return reportExprError(endLocation, {}, "Double braces are not permitted within interpolated strings; did you mean '\\{'?");
        case Lexeme::BrokenString:
            nextLexeme();
            return reportExprError(endLocation, {}, "Malformed interpolated string; did you forget to add a '}'?");
        default:
            return reportExprError(endLocation, {}, "Malformed interpolated string, got %s", lexer.current().toString().c_str());
        }
    } while (true);

    AstArray<AstArray<char>> stringsArray = copy(strings);
    AstArray<AstExpr*> expressionsArray = copy(expressions);
    AstExprInterpString* node = allocator.alloc<AstExprInterpString>(Location{startLocation, endLocation}, stringsArray, expressionsArray);
    if (options.storeCstData)
        cstNodeMap[node] = allocator.alloc<CstExprInterpString>(copy(sourceStrings), copy(stringPositions));
    return node;
}

AstExpr* Parser::parseNumber()
{
    Location start = lexer.current().location;

    scratchData.assign(lexer.current().data, lexer.current().getLength());
    AstArray<char> sourceData;
    if (options.storeCstData)
        sourceData = copy(scratchData);

    // Remove all internal _ - they don't hold any meaning and this allows parsing code to just pass the string pointer to strtod et al
    if (scratchData.find('_') != std::string::npos)
    {
        scratchData.erase(std::remove(scratchData.begin(), scratchData.end(), '_'), scratchData.end());
    }

    double value = 0;
    ConstantNumberParseResult result = parseDouble(value, scratchData.c_str());
    nextLexeme();

    if (result == ConstantNumberParseResult::Malformed)
        return reportExprError(start, {}, "Malformed number");

    AstExprConstantNumber* node = allocator.alloc<AstExprConstantNumber>(start, value, result);
    if (options.storeCstData)
        cstNodeMap[node] = allocator.alloc<CstExprConstantNumber>(sourceData);
    return node;
}

AstLocal* Parser::pushLocal(const Binding& binding)
{
    const Name& name = binding.name;
    AstLocal*& local = localMap[name.name];

    local = allocator.alloc<AstLocal>(
        name.name, name.location, /* shadow= */ local, functionStack.size() - 1, functionStack.back().loopDepth, binding.annotation
    );

    localStack.push_back(local);

    return local;
}

unsigned int Parser::saveLocals()
{
    return unsigned(localStack.size());
}

void Parser::restoreLocals(unsigned int offset)
{
    for (size_t i = localStack.size(); i > offset; --i)
    {
        AstLocal* l = localStack[i - 1];

        localMap[l->name] = l->shadow;
    }

    localStack.resize(offset);
}

bool Parser::expectAndConsume(char value, const char* context)
{
    return expectAndConsume(static_cast<Lexeme::Type>(static_cast<unsigned char>(value)), context);
}

bool Parser::expectAndConsume(Lexeme::Type type, const char* context)
{
    if (lexer.current().type != type)
    {
        expectAndConsumeFail(type, context);

        // check if this is an extra token and the expected token is next
        if (lexer.lookahead().type == type)
        {
            // skip invalid and consume expected
            nextLexeme();
            nextLexeme();
        }

        return false;
    }
    else
    {
        nextLexeme();
        return true;
    }
}

// LUAU_NOINLINE is used to limit the stack cost of this function due to std::string objects, and to increase caller performance since this code is
// cold
LUAU_NOINLINE void Parser::expectAndConsumeFail(Lexeme::Type type, const char* context)
{
    std::string typeString = Lexeme(Location(Position(0, 0), 0), type).toString();
    std::string currLexemeString = lexer.current().toString();

    if (context)
        report(lexer.current().location, "Expected %s when parsing %s, got %s", typeString.c_str(), context, currLexemeString.c_str());
    else
        report(lexer.current().location, "Expected %s, got %s", typeString.c_str(), currLexemeString.c_str());
}

bool Parser::expectMatchAndConsume(char value, const MatchLexeme& begin, bool searchForMissing)
{
    Lexeme::Type type = static_cast<Lexeme::Type>(static_cast<unsigned char>(value));

    if (lexer.current().type != type)
    {
        expectMatchAndConsumeFail(type, begin);

        return expectMatchAndConsumeRecover(value, begin, searchForMissing);
    }
    else
    {
        nextLexeme();

        return true;
    }
}

LUAU_NOINLINE bool Parser::expectMatchAndConsumeRecover(char value, const MatchLexeme& begin, bool searchForMissing)
{
    Lexeme::Type type = static_cast<Lexeme::Type>(static_cast<unsigned char>(value));

    if (searchForMissing)
    {
        // previous location is taken because 'current' lexeme is already the next token
        unsigned currentLine = lexer.previousLocation().end.line;

        // search to the end of the line for expected token
        // we will also stop if we hit a token that can be handled by parsing function above the current one
        Lexeme::Type lexemeType = lexer.current().type;

        while (currentLine == lexer.current().location.begin.line && lexemeType != type && matchRecoveryStopOnToken[lexemeType] == 0)
        {
            nextLexeme();
            lexemeType = lexer.current().type;
        }

        if (lexemeType == type)
        {
            nextLexeme();

            return true;
        }
    }
    else
    {
        // check if this is an extra token and the expected token is next
        if (lexer.lookahead().type == type)
        {
            // skip invalid and consume expected
            nextLexeme();
            nextLexeme();

            return true;
        }
    }

    return false;
}

// LUAU_NOINLINE is used to limit the stack cost of this function due to std::string objects, and to increase caller performance since this code is
// cold
LUAU_NOINLINE void Parser::expectMatchAndConsumeFail(Lexeme::Type type, const MatchLexeme& begin, const char* extra)
{
    std::string typeString = Lexeme(Location(Position(0, 0), 0), type).toString();
    std::string matchString = Lexeme(Location(Position(0, 0), 0), begin.type).toString();

    if (lexer.current().location.begin.line == begin.position.line)
        report(
            lexer.current().location,
            "Expected %s (to close %s at column %d), got %s%s",
            typeString.c_str(),
            matchString.c_str(),
            begin.position.column + 1,
            lexer.current().toString().c_str(),
            extra ? extra : ""
        );
    else
        report(
            lexer.current().location,
            "Expected %s (to close %s at line %d), got %s%s",
            typeString.c_str(),
            matchString.c_str(),
            begin.position.line + 1,
            lexer.current().toString().c_str(),
            extra ? extra : ""
        );
}

bool Parser::expectMatchEndAndConsume(Lexeme::Type type, const MatchLexeme& begin)
{
    if (lexer.current().type != type)
    {
        expectMatchEndAndConsumeFail(type, begin);

        // check if this is an extra token and the expected token is next
        if (lexer.lookahead().type == type)
        {
            // skip invalid and consume expected
            nextLexeme();
            nextLexeme();

            return true;
        }

        return false;
    }
    else
    {
        // If the token matches on a different line and a different column, it suggests misleading indentation
        // This can be used to pinpoint the problem location for a possible future *actual* mismatch
        if (lexer.current().location.begin.line != begin.position.line && lexer.current().location.begin.column != begin.position.column &&
            endMismatchSuspect.position.line < begin.position.line) // Only replace the previous suspect with more recent suspects
        {
            endMismatchSuspect = begin;
        }

        nextLexeme();

        return true;
    }
}

// LUAU_NOINLINE is used to limit the stack cost of this function due to std::string objects, and to increase caller performance since this code is
// cold
LUAU_NOINLINE void Parser::expectMatchEndAndConsumeFail(Lexeme::Type type, const MatchLexeme& begin)
{
    if (endMismatchSuspect.type != Lexeme::Eof && endMismatchSuspect.position.line > begin.position.line)
    {
        std::string matchString = Lexeme(Location(Position(0, 0), 0), endMismatchSuspect.type).toString();
        std::string suggestion = format("; did you forget to close %s at line %d?", matchString.c_str(), endMismatchSuspect.position.line + 1);

        expectMatchAndConsumeFail(type, begin, suggestion.c_str());
    }
    else
    {
        expectMatchAndConsumeFail(type, begin);
    }
}

template<typename T>
AstArray<T> Parser::copy(const T* data, size_t size)
{
    AstArray<T> result;

    result.data = size ? static_cast<T*>(allocator.allocate(sizeof(T) * size)) : nullptr;
    result.size = size;

    // This is equivalent to std::uninitialized_copy, but without the exception guarantee
    // since our types don't have destructors
    for (size_t i = 0; i < size; ++i)
        new (result.data + i) T(data[i]);

    return result;
}

template<typename T>
AstArray<T> Parser::copy(const TempVector<T>& data)
{
    return copy(data.empty() ? nullptr : &data[0], data.size());
}

template<typename T>
AstArray<T> Parser::copy(std::initializer_list<T> data)
{
    return copy(data.size() == 0 ? nullptr : data.begin(), data.size());
}

AstArray<char> Parser::copy(const std::string& data)
{
    AstArray<char> result = copy(data.c_str(), data.size() + 1);

    result.size = data.size();

    return result;
}

void Parser::incrementRecursionCounter(const char* context)
{
    recursionCounter++;

    if (recursionCounter > unsigned(FInt::LuauRecursionLimit))
    {
        ParseError::raise(lexer.current().location, "Exceeded allowed recursion depth; simplify your %s to make the code compile", context);
    }
}

void Parser::report(const Location& location, const char* format, va_list args)
{
    // To reduce number of errors reported to user for incomplete statements, we skip multiple errors at the same location
    // For example, consider 'local a = (((b + ' where multiple tokens haven't been written yet
    if (!parseErrors.empty() && location == parseErrors.back().getLocation())
        return;

    std::string message = vformat(format, args);

    // when limited to a single error, behave as if the error recovery is disabled
    if (FInt::LuauParseErrorLimit == 1)
        throw ParseError(location, message);

    parseErrors.emplace_back(location, message);

    if (parseErrors.size() >= unsigned(FInt::LuauParseErrorLimit) && !options.noErrorLimit)
        ParseError::raise(location, "Reached error limit (%d)", int(FInt::LuauParseErrorLimit));
}

void Parser::report(const Location& location, const char* format, ...)
{
    va_list args;
    va_start(args, format);
    report(location, format, args);
    va_end(args);
}

LUAU_NOINLINE void Parser::reportNameError(const char* context)
{
    if (context)
        report(lexer.current().location, "Expected identifier when parsing %s, got %s", context, lexer.current().toString().c_str());
    else
        report(lexer.current().location, "Expected identifier, got %s", lexer.current().toString().c_str());
}

AstStatError* Parser::reportStatError(
    const Location& location,
    const AstArray<AstExpr*>& expressions,
    const AstArray<AstStat*>& statements,
    const char* format,
    ...
)
{
    va_list args;
    va_start(args, format);
    report(location, format, args);
    va_end(args);

    return allocator.alloc<AstStatError>(location, expressions, statements, unsigned(parseErrors.size() - 1));
}

AstExprError* Parser::reportExprError(const Location& location, const AstArray<AstExpr*>& expressions, const char* format, ...)
{
    va_list args;
    va_start(args, format);
    report(location, format, args);
    va_end(args);

    return allocator.alloc<AstExprError>(location, expressions, unsigned(parseErrors.size() - 1));
}

AstTypeError* Parser::reportTypeError(const Location& location, const AstArray<AstType*>& types, const char* format, ...)
{
    va_list args;
    va_start(args, format);
    report(location, format, args);
    va_end(args);

    return allocator.alloc<AstTypeError>(location, types, false, unsigned(parseErrors.size() - 1));
}

AstTypeError* Parser::reportMissingTypeError(const Location& parseErrorLocation, const Location& astErrorLocation, const char* format, ...)
{
    va_list args;
    va_start(args, format);
    report(parseErrorLocation, format, args);
    va_end(args);

    return allocator.alloc<AstTypeError>(astErrorLocation, AstArray<AstType*>{}, true, unsigned(parseErrors.size() - 1));
}

void Parser::nextLexeme()
{
    Lexeme::Type type = lexer.next(/* skipComments= */ false, true).type;

    while (type == Lexeme::BrokenComment || type == Lexeme::Comment || type == Lexeme::BlockComment)
    {
        const Lexeme& lexeme = lexer.current();

        if (options.captureComments)
            commentLocations.push_back(Comment{lexeme.type, lexeme.location});

        // Subtlety: Broken comments are weird because we record them as comments AND pass them to the parser as a lexeme.
        // The parser will turn this into a proper syntax error.
        if (lexeme.type == Lexeme::BrokenComment)
            return;

        // Comments starting with ! are called "hot comments" and contain directives for type checking / linting / compiling
        if (lexeme.type == Lexeme::Comment && lexeme.getLength() && lexeme.data[0] == '!')
        {
            const char* text = lexeme.data;

            unsigned int end = lexeme.getLength();
            while (end > 0 && isSpace(text[end - 1]))
                --end;

            hotcomments.push_back({hotcommentHeader, lexeme.location, std::string(text + 1, text + end)});
        }

        type = lexer.next(/* skipComments= */ false, /* updatePrevLocation= */ false).type;
    }
}

} // namespace Luau
