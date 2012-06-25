/* A Bison parser, made by GNU Bison 2.5.  */

/* Skeleton interface for Bison GLR parsers in C
   
      Copyright (C) 2002-2011 Free Software Foundation, Inc.
   
   This program is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.
   
   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.
   
   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.  */

/* As a special exception, you may create a larger work that contains
   part or all of the Bison parser skeleton and distribute that work
   under terms of your choice, so long as that work isn't itself a
   parser generator using the skeleton or a modified version thereof
   as a parser skeleton.  Alternatively, if you modify or redistribute
   the parser skeleton itself, you may (at your option) remove this
   special exception, which will cause the skeleton and the resulting
   Bison output files to be licensed under the GNU General Public
   License without this special exception.
   
   This special exception was added by the Free Software Foundation in
   version 2.2 of Bison.  */

/* "%code requires" blocks.  */

/* Line 2663 of glr.c  */
#line 14 "./parser.y"

    
#ifndef BISON_HEADER
#define BISON_HEADER
    
#include "util.hh"
    
#include "nixexpr.hh"
#include "eval.hh"

namespace nix {

    struct ParseData 
    {
        EvalState & state;
        SymbolTable & symbols;
        Expr * result;
        Path basePath;
        Path path;
        string error;
        Symbol sLetBody;
        ParseData(EvalState & state)
            : state(state)
            , symbols(state.symbols)
            , sLetBody(symbols.create("<let-body>"))
            { };
    };
    
}

#define YY_DECL int yylex \
    (YYSTYPE * yylval_param, YYLTYPE * yylloc_param, yyscan_t yyscanner, nix::ParseData * data)

#endif




/* Line 2663 of glr.c  */
#line 77 "parser-tab.hh"

/* Tokens.  */
#ifndef YYTOKENTYPE
# define YYTOKENTYPE
   /* Put the tokens into the symbol table, so that GDB and other debuggers
      know about them.  */
   enum yytokentype {
     ID = 258,
     ATTRPATH = 259,
     STR = 260,
     IND_STR = 261,
     INT = 262,
     PATH = 263,
     SPATH = 264,
     URI = 265,
     IF = 266,
     THEN = 267,
     ELSE = 268,
     ASSERT = 269,
     WITH = 270,
     LET = 271,
     IN = 272,
     REC = 273,
     INHERIT = 274,
     EQ = 275,
     NEQ = 276,
     AND = 277,
     OR = 278,
     IMPL = 279,
     OR_KW = 280,
     DOLLAR_CURLY = 281,
     IND_STRING_OPEN = 282,
     IND_STRING_CLOSE = 283,
     ELLIPSIS = 284,
     UPDATE = 285,
     NEG = 286,
     CONCAT = 287
   };
#endif


#ifndef YYSTYPE
typedef union YYSTYPE
{

/* Line 2663 of glr.c  */
#line 234 "./parser.y"

  // !!! We're probably leaking stuff here.  
  nix::Expr * e;
  nix::ExprList * list;
  nix::ExprAttrs * attrs;
  nix::Formals * formals;
  nix::Formal * formal;
  int n;
  char * id; // !!! -> Symbol
  char * path;
  char * uri;
  std::vector<nix::Symbol> * attrNames;
  std::vector<nix::Expr *> * string_parts;



/* Line 2663 of glr.c  */
#line 142 "parser-tab.hh"
} YYSTYPE;
# define YYSTYPE_IS_TRIVIAL 1
#endif

#if ! defined YYLTYPE && ! defined YYLTYPE_IS_DECLARED
typedef struct YYLTYPE
{

  int first_line;
  int first_column;
  int last_line;
  int last_column;

} YYLTYPE;
# define YYLTYPE_IS_DECLARED 1
# define YYLTYPE_IS_TRIVIAL 1
#endif








