#!/usr/bin/env python

import re

class Token:
    COMMENT=1
    SPACE=2
    IDENTIFIER=3
    STRING=4
    BLOB=5
    OPERATOR=6
    NUMBER=7
    KEYWORD=8
    UNRECOGNIZED=9
    END=10   # end of the input
    SEMICOLON=11

    def type_str(self):
        return self._num_to_name_map[self.type]

    def __str__(self):
        return "%d:%d(%s)%s: %s%s%s" % (self.start_line,
                                        self.start_column, self.type_str(),
                                        " "+self.error if self.error else "",
                                        self.source,
                                        "\n " if self.value is not None else "",
                                        repr(self.value) if self.value is not None else "")

Token._num_to_name_map={}
for i in dir(Token):
    if not i.startswith("_") and i.upper()==i:
        Token._num_to_name_map[getattr(Token, i)]=i

def tokenize(sql, include_space=False):
    class state:
        line=1
        column=1
        epos=-1
        pos=0
        error=None

    def fixup(toktype, value=None):
        err=state.error
        state.error=False

        tok=Token()
        tok.type=toktype
        tok.start_line=state.line
        tok.start_column=state.column
        tok.start_pos=state.pos
        tok.error=err

        if state.epos<0:
            state.epos=len(sql)

        tok.source=sql[state.pos:state.epos]
        tok.source_upper=tok.source.upper()
        tok.value=value
        if isinstance(value, basestring):
            tok.value_upper=value.upper()

        # update line/column
        nline=tok.source.count("\n")
        if nline:
            state.line+=nline
            state.column=1+tok.source.rindex("\n")+1
        else:
            state.column+=len(tok.source)

        tok.end_line=state.line
        tok.end_column=state.column
        tok.end_pos=state.epos

        state.pos=state.epos
        state.epos=-1

        return tok

    WHITESPACE=" \t\n\r\f"

    QUOTES      ="'\"[`"
    QUOTES_END  ="'\"]`"
    QUOTES_QUOTE=(True, True, False, False)

    ONE_CHAR_OPERATORS="-()+*/%=<>|,&~."
    TWO_CHAR_OPERATORS=(
        "<=", ">=", "<<", ">>", "<>", "!=", "||"
    )
    TWO_CHAR_FIRST=""
    for i in TWO_CHAR_OPERATORS:
        if i not in TWO_CHAR_FIRST:
            TWO_CHAR_FIRST+=i

    IDCHAR="01234567890abcdefghijklmnopqrstuvwxyz_$"  # plus all codepoints >= 128

    # Same method name as SQLite source - different mechanism
    def IsIdChar(x):
        return x in IDCHAR or ord(x)>=128

    # https://www.sqlite.org/lang_keywords.html
    KEYWORDS="""
ABORT ACTION ADD AFTER ALL ALTER ANALYZE AND AS ASC ATTACH
AUTOINCREMENT BEFORE BEGIN BETWEEN BY CASCADE CASE CAST CHECK COLLATE
COLUMN COMMIT CONFLICT CONSTRAINT CREATE CROSS CURRENT_DATE
CURRENT_TIME CURRENT_TIMESTAMP DATABASE DEFAULT DEFERRABLE DEFERRED
DELETE DESC DETACH DISTINCT DROP EACH ELSE END ESCAPE EXCEPT EXCLUSIVE
EXISTS EXPLAIN FAIL FOR FOREIGN FROM FULL GLOB GROUP HAVING IF IGNORE
IMMEDIATE IN INDEX INDEXED INITIALLY INNER INSERT INSTEAD INTERSECT
INTO IS ISNULL JOIN KEY LEFT LIKE LIMIT MATCH NATURAL NO NOT NOTNULL
NULL OF OFFSET ON OR ORDER OUTER PLAN PRAGMA PRIMARY QUERY RAISE
RECURSIVE REFERENCES REGEXP REINDEX RELEASE RENAME REPLACE RESTRICT
RIGHT ROLLBACK ROW SAVEPOINT SELECT SET TABLE TEMP TEMPORARY THEN TO
TRANSACTION TRIGGER UNION UNIQUE UPDATE USING VACUUM VALUES VIEW
VIRTUAL WHEN WHERE WITH WITHOUT
""".split()

    numberpat=re.compile(r"""
    # https://docs.python.org/2/library/re.html#simulating-scanf
    # leading +/- is dealt with separately

    # floating point number
    (?P<number>(\d+(\.\d*)?|\.\d+)([eE][-+]?\d+)?)
    |
    # 0x or 0 octal style
    (?P<base>0[xX][\dA-Fa-f]+|0[0-7]*|\d+)
    """, re.VERBOSE)

    while state.pos<len(sql):
        if sql[state.pos:state.pos+2]=="--":
            state.epos=sql.find("\n", state.pos+2)
            if state.epos>0:
                state.epos+=1
            yield fixup(Token.COMMENT)
            continue
        if sql[state.pos:state.pos+2]=="/*":
            state.epos=sql.find("*/", state.pos+2)
            if state.epos<0:
                state.error="Unterminated comment"
            yield fixup(Token.COMMENT)
            continue
        if sql[state.pos]==";":
            state.epos=state.pos+1
            yield fixup(Token.SEMICOLON)
            continue
        if sql[state.pos] in WHITESPACE:
            state.epos=state.pos
            while state.epos<len(sql) and sql[state.epos] in WHITESPACE:
                state.epos+=1
            r=fixup(Token.SPACE)
            if include_space:
                yield r
            continue
        if sql[state.pos] in QUOTES:
            q=sql[state.pos]
            i=QUOTES.find(q)
            eq=QUOTES_END[i]
            qq=QUOTES_QUOTE[i]
            i=state.pos+1
            value=""
            while True:
                j=sql.find(eq, i)
                if j<0:
                    state.epos=-1
                    value+=sql[i:]
                    state.error="Unterminated quotes (%s)" % q
                    break
                value+=sql[i:j]
                if qq and (j+1<len(sql) and sql[j+1]==q):
                    value+=q
                    i=j+2
                    continue
                state.epos=j+1
                break

            yield fixup(Token.STRING if q=="'" else Token.IDENTIFIER, value)
            continue
        if sql[state.pos:state.pos+2].lower()=="x'":
            i=sql.find("'", state.pos+2)
            value=None
            if i<0:
                state.error="Unterminated blob literal"
            else:
                state.epos=i+1
                hex=sql[state.pos+2:i]
                if len(hex)%2!=0:
                    state.error="Odd number of hex digits"
                else:
                    value=""
                    try:
                        for i in range(state.pos+2, i, 2):
                            value+=chr(int(sql[i:i+2], 16))
                    except ValueError:
                        value=None
                        state.error="Not a valid hex string component \"%s\"" % sql[i:i+2]
            yield fixup(Token.BLOB, value)
            continue
        if sql[state.pos] in "0123456789" or (
                sql[state.pos]=='.' and state.pos+1<len(sql) and sql[state.pos+1] in "0123456789"):
            mo=numberpat.match(sql[state.pos:])
            assert mo
            if mo.group("number") is not None:
                s=mo.group("number")
                value=float(s) if "." in s else long(s)
            else:
                s=mo.group("base")
                base=10
                if s.lower().startswith("0x"):
                    base=16
                elif s.startswith("0"):
                    base=8
                value=long(s, base)
            state.epos=state.pos+len(s)
            yield fixup(Token.NUMBER, value)
            if state.pos<len(sql) and IsIdChar(sql[state.pos]):
                state.epos=state.pos
                while state.epos<len(sql) and IsIdChar(sql[state.epos]):
                    state.epos+=1
                yield fixup(Token.UNRECOGNIZED)
            continue

        # ::TODO:: bindings (? $ @ # :)

        if sql[state.pos] in TWO_CHAR_FIRST:
            c=sql[state.pos:state.pos+2]
            if c in TWO_CHAR_OPERATORS:
                state.epos=state.pos+2
                yield fixup(Token.OPERATOR, c)
                continue
        if sql[state.pos] in ONE_CHAR_OPERATORS:
            state.epos=state.pos+1
            yield fixup(Token.OPERATOR, sql[state.pos])
            continue

        c=sql[state.pos]
        if not IsIdChar(c):
            state.epos=state.pos+1
            yield fixup(Token.UNRECOGNIZED)
            continue

        state.epos=state.pos+1
        while state.epos<len(sql) and IsIdChar(sql[state.epos]):
            state.epos+=1
        value=sql[state.pos:state.epos]
        vu=value.upper()
        if vu in KEYWORDS:
            yield fixup(Token.KEYWORD, vu)
        else:
            yield fixup(Token.IDENTIFIER, value)

    state.epos=state.pos
    yield fixup(Token.END)


def tokenize_debug(sql):
    for tok in tokenize(sql):
        print tok

def parse_debug(sql):
    import pprint
    for node in parse(sql):
        pprint.pprint(node)

class Node:
    pass

def parse(sql):

    _tokens=[]
    _tokenizer=tokenize(sql)

    # highest first
    binop_precedence=(
        ("||",),
        ("*", "/", "%"),
        ("+", "-"),
        ("<<", ">>", "&", "|"),
        ("<", "<=", ">", ">="),
        ("=", "==", "!=", "<>", "IS", "IS NOT", "IN", "LIKE", "GLOB", "MATCH", "REGEXP"),
        ("AND",),
        ("OR",),
    )

    precedences={}
    for level, row in enumerate(binop_precedence[::-1]):
        for item in row:
            precedences[item]=2+level

    # all others are left associative
    binop_right_associativity=("NOT", "ESCAPE", "~")

    unary_predences={
        "-": precedences["*"],
        "+": precedences["*"],
    }

    def cur_token():
        if not _tokens:
            _tokens.append(_tokenizer.next())
        return _tokens[0]

    def peek_token(ahead=1):
        while len(_tokens)<ahead+1:
            _tokens.append(_tokenizer.next())
        return _tokens[ahead]

    def advance():
        return _tokens.pop(0)

    def is_keyword(t, kw):
        return t.type==Token.KEYWORD and t.value_upper==kw.upper()

    def is_operator(t, op):
        return t.type==Token.OPERATOR and t.value==op

    def is_identifier(t):
        return t.type==Token.IDENTIFIER
        # or double quoted string that doesn't reference anything else?

    def parse_result_column():
        tok=cur_token()
        if is_operator(tok, "*"):
            advance()
            return {"*": "*", "token": tok}
        if is_identifier(tok) and is_operator(peek_token(1), ".") and is_operator(peek_token(2), "*"):
            advance()
            advance()
            advance()
            return {"table_dot_star": tok.value, "tokens": []}
        e=parse_expr()
        n=None
        if is_keyword(cur_token(), "AS"):
            advance()
            if not is_identifier(cur_token()):
                parse_error("Expecting name for result column")
            n=advance().value
        elif is_identifier(cur_token()):
            n=advance().value
        return {"expr": e, "name": n}

    def parse_expr(precedence=1):
        tree=parse_expr_primary()

        while True:
            tok=cur_binop_token()
            if not tok or precedences[tok.value]<precedence:
                return tree
            advance()
            rhs=parse_expr(precedence+ (tok.value not in binop_right_associativity))
            tree={"binop": tok.value, "left": tree, "right": rhs}

    def parse_expr_primary():
        tok=cur_token()
        if tok.type in (Token.STRING, Token.BLOB, Token.NUMBER):
            advance()
            return {"literal": tok.value}
        if is_operator(tok, "("):
            advance()
            val=parse_expr()
            closetok=cur_token()
            if not is_operator(closetok, ")"):
                parse_error("Expected closing )", start=tok)
            advance()
            return {"expr": val}
        if tok.type==Token.OPERATOR:
            if tok.value in unary_predences:
                advance()
                return {"unary": tok.value, "value": parse_expr(unary_predences[tok.value])}
        if tok.type==Token.IDENTIFIER:
            return {"id": advance().value}

        parse_error("Unexpected token")

    def cur_binop_token():
        # returns current binary operation token.  in addition to
        # regular operators like plus and divide, it also looks for
        # ascii based ones like IS & IS NOT
        tok=cur_token()
        if tok.type==Token.OPERATOR:
            if tok.value in precedences:
                return tok
            return None
        # ::TODO:: look for IS IS NOT etc
        return None


    def parse_select():
        node={}
        tok=cur_token()
        assert is_keyword(tok, "SELECT")
        advance()
        tok=cur_token()
        node["DISTINCT"]=is_keyword(tok, "DISTINCT")
        node["ALL"]=is_keyword(tok, "ALL")
        if node["DISTINCT"] or node["ALL"]:
            advance()
        rescols=[]
        rescols.append(parse_result_column())
        while is_operator(cur_token(), ","):
            advance()
            rescols.append(parse_result_column())
        node["result-columns"]=rescols
        # FROM/WHERE/GROUP BY/VALUES
        return node

    def parse_error(message, start=None):
        tok=cur_token()
        print "Error:", message
        print "At", unicode(tok)
        if start:
            print "Started at", unicode(start)
        while cur_token().type not in (Token.END, Token.SEMICOLON):
            advance()
        1/0

    while True:
        tok=cur_token()
        if tok.type==Token.END:
            return
        if tok.type==Token.SEMICOLON:
            advance()
            continue

        assert is_keyword(cur_token(), "SELECT")
        yield parse_select()

        # try/catch to move to END/SEMICOLON

        if cur_token().type not in (Token.END, Token.SEMICOLON):
            parse_error("unexpected token")
            return

        advance()
