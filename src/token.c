/*
 * Copyright 2026 TeamMyonlang
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "token.h"

const char *token_type_name(TokenType type) {
    switch (type) {
        case TOK_INT:          return "INT";
        case TOK_FLOAT:        return "FLOAT";
        case TOK_STRING:       return "STRING";
        case TOK_IDENT:        return "IDENT";
        case TOK_KW_SYSTEM:    return "system";
        case TOK_KW_MODULE:    return "module";
        case TOK_KW_MYON:      return "myon";
        case TOK_KW_RET:       return "ret";
        case TOK_KW_STR:       return "str";
        case TOK_KW_CHAR:      return "char";
        case TOK_KW_INT:       return "int";
        case TOK_KW_FLOAT:     return "float";
        case TOK_KW_BOOL:      return "bool";
        case TOK_KW_VOID:      return "void";
        case TOK_KW_ERROR:     return "error";
        case TOK_KW_THEN:      return "then";
        case TOK_KW_RANGE:     return "range";
        case TOK_KW_SELF:      return "self";
        case TOK_KW_AS:        return "as";
        case TOK_KW_TRUE:      return "true";
        case TOK_KW_FALSE:     return "false";
        case TOK_MYON_IF:      return "myon.if";
        case TOK_MYON_ELIF:    return "myon.elif";
        case TOK_MYON_ELSE:    return "myon.else";
        case TOK_MYON_WHILE:   return "myon.while";
        case TOK_MYON_FOR:     return "myon.for";
        case TOK_MYON_IN:      return "myon.in";
        case TOK_MYON_BREAK:   return "myon.break";
        case TOK_MYON_CONTINUE:return "myon.continue";
        case TOK_MYON_FUNC:    return "myon.func";
        case TOK_MYON_STRUCT:  return "myon.struct";
        case TOK_MYON_ARRAY:   return "myon.array";
        case TOK_MYON_MAP:     return "myon.map";
        case TOK_MYON_PRINT:   return "myon.print";
        case TOK_MYON_INPUT:   return "myon.input";
        case TOK_MYON_AND:     return "myon.and";
        case TOK_MYON_OR:      return "myon.or";
        case TOK_MYON_NOT:     return "myon.not";
        case TOK_MYON_NIL:     return "myon.nil";
        case TOK_MYON_EXPOSE:  return "myon.expose";
        case TOK_MYON_LAMBDA:  return "myon.lambda";
        case TOK_MYON_EXTENDS: return "myon.extends";
        case TOK_MYON_ASYNC:   return "myon.async";
        case TOK_MYON_AWAIT:   return "myon.await";
        case TOK_EQ:           return "==";
        case TOK_NEQ:          return "!=";
        case TOK_LT:           return "<";
        case TOK_GT:           return ">";
        case TOK_LE:           return "<=";
        case TOK_GE:           return ">=";
        case TOK_PLUS:         return "+";
        case TOK_MINUS:        return "-";
        case TOK_STAR:         return "*";
        case TOK_SLASH:        return "/";
        case TOK_PLUS_EQ:      return "+=";
        case TOK_MINUS_EQ:     return "-=";
        case TOK_STAR_EQ:      return "*=";
        case TOK_SLASH_EQ:     return "/=";
        case TOK_ASSIGN:       return "=";
        case TOK_DOT:          return ".";
        case TOK_COMMA:        return ",";
        case TOK_COLON:        return ":";
        case TOK_SEMICOLON:    return ";";
        case TOK_LPAREN:       return "(";
        case TOK_RPAREN:       return ")";
        case TOK_LBRACE:       return "{";
        case TOK_RBRACE:       return "}";
        case TOK_LBRACKET:     return "[";
        case TOK_RBRACKET:     return "]";
        case TOK_NEWLINE:      return "NEWLINE";
        case TOK_EOF:          return "EOF";
    }
    return "UNKNOWN";
}
