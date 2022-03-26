// This file is part of Notepad2.
// See License.txt for details about distribution and modification.
//! Lexer for Python.

#include <cassert>
#include <cstring>

#include <string>
#include <string_view>
#include <vector>

#include "ILexer.h"
#include "Scintilla.h"
#include "SciLexer.h"

#include "WordList.h"
#include "LexAccessor.h"
#include "Accessor.h"
#include "StyleContext.h"
#include "CharacterSet.h"
#include "StringUtils.h"
#include "LexerModule.h"

using namespace Lexilla;

namespace {

constexpr bool IsPyString(int state) noexcept {
	return state < SCE_PY_BYTES_SQ;
}

constexpr bool IsPyFormattedString(int state) noexcept {
	return state >= SCE_PY_FMTSTRING_SQ && state <= SCE_PY_TRIPLE_RAWFMTSTRING_DQ;
}

constexpr bool IsPyDoubleQuotedString(int state) noexcept {
	return state & true;
}

constexpr bool IsPyTripleQuotedString(int state) noexcept {
	return (state & 3) > 1;
}

constexpr bool IsPyRawString(int state) noexcept {
	return (state & 7) > 3;
}

constexpr bool IsPyStringStyle(int state) noexcept {
	return state >= SCE_PY_STRING_SQ;
}

struct EscapeSequence {
	int outerState = SCE_PY_DEFAULT;
	int digitsLeft = 0;
	int numBase = 16;

	// no highlight for name in '\N{name}'.
	bool resetEscapeState(int state, int chNext) noexcept {
		outerState = state;
		digitsLeft = 0;
		numBase = 16;
		if (chNext == 'x') {
			digitsLeft = 3;
		} else if (IsOctalDigit(chNext)) {
			digitsLeft = 3;
			numBase = 8;
		} else if (AnyOf(chNext, '\\', '\'', '"', 'a', 'b', 'f', 'n', 'r', 't', 'v')) {
			digitsLeft = 1;
		} else if (IsPyString(state)) {
			if (chNext == 'u') {
				digitsLeft = 5;
			} else if (chNext == 'U') {
				digitsLeft = 9;
			} else if (chNext == 'N') {
				digitsLeft = 1;
			}
		}
		return digitsLeft != 0;
	}
	bool atEscapeEnd(int ch) noexcept {
		--digitsLeft;
		return digitsLeft <= 0 || !IsADigit(ch, numBase);
	}
};

struct FormattedStringState {
	int state;
	int nestedLevel;
	int parenCount;
};

constexpr bool IsPyStringPrefix(int ch) noexcept {
	return AnyOf(ch, 'r', 'b', 'f', 'u', 'R', 'B', 'F', 'U');
}

inline void EnterPyStringState(StyleContext &sc) {
	int ch = sc.ch | 0x20;
	int chNext = sc.chNext;
	const int next = chNext | 0x20;
	int state = SCE_PY_IDENTIFIER;
	int offset = 1;
	if (ch == 'r') {
		if (next == 'b' || next == 'f' || next == 'u') {
			ch = next;
			offset = 2;
			chNext = sc.GetRelative(2);
		}
	} else if (next == 'r') {
		offset = 2;
		chNext = sc.GetRelative(2);
	}
	if (chNext == '\'' || chNext == '\"') {
		switch (ch) {
		case 'r':
			state = SCE_PY_RAWSTRING_SQ;
			break;
		case 'f':
			state = SCE_PY_FMTSTRING_SQ;
			break;
		case 'b':
			state = SCE_PY_BYTES_SQ;
			break;
		default:
			state = SCE_PY_STRING_SQ;
			break;
		}
		if (chNext == '\"') {
			state += SCE_PY_STRING_DQ - SCE_PY_STRING_SQ;
		}
		if (offset == 2) {
			state += SCE_PY_RAWSTRING_SQ - SCE_PY_STRING_SQ;
		}
		if (sc.GetRelative(offset + 1) == chNext && sc.GetRelative(offset + 2) == chNext) {
			offset += 2;
			state += SCE_PY_TRIPLE_RAWSTRING_SQ - SCE_PY_RAWSTRING_SQ;
		}
	} else {
		--offset;
	}

	sc.SetState(state);
	sc.Advance(offset);
}

// https://docs.python.org/3/library/stdtypes.html#printf-style-string-formatting
// https://docs.python.org/3/library/stdtypes.html#printf-style-bytes-formatting

constexpr bool IsDateTimeFormatSpecifier(int ch) noexcept {
	// https://docs.python.org/3/library/datetime.html#strftime-and-strptime-behavior
	return AnyOf(ch, 'a', 'A',
					'b', 'B',
					'c',
					'd',
					'f',
					'H',
					'I',
					'j',
					'm', 'M',
					'p',
					'S',
					'U',
					'w', 'W',
					'x', 'X',
					'y', 'Y',
					'z', 'Z',
					// Python 3.6
					'G',
					'u',
					'V');
}

constexpr bool IsPercentFormatSpecifier(char ch) noexcept {
	return AnyOf(ch, 'd',
					'i',
					'o',
					'u',
					'x', 'X',
					'e', 'E',
					'f', 'F',
					'g', 'G',
					'c',
					'b',
					's',
					'a',
					'r');
}

constexpr bool IsInvalidMappingKey(char ch) noexcept {
	// restrict mapping key: excludes C0 and some special handled characters
	return (ch >= 0 && ch < 32) || AnyOf(ch, '(', ')', '\'', '\"', '\\', '{', '}', '%', '$', '\x7F');
}

Sci_Position CheckPercentFormatSpecifier(const StyleContext &sc, LexAccessor &styler, Sci_Position &keyLen, bool insideUrl) noexcept {
	if (sc.chNext == '%') {
		return 2;
	}
	if (insideUrl && IsHexDigit(sc.chNext)) {
		// percent encoded URL string
		return 0;
	}
	if (IsASpaceOrTab(sc.chNext) && IsADigit(sc.chPrev)) {
		// ignore word after percent: "5% x"
		return 0;
	}
	if (IsDateTimeFormatSpecifier(sc.chNext)) {
		return 2;
	}

	Sci_PositionU pos = sc.currentPos + 1;
	char ch = styler.SafeGetCharAt(pos);
	// 2. (optional) Mapping key
	if (ch == '(') {
		ch = styler.SafeGetCharAt(++pos);
		while (!IsInvalidMappingKey(ch)) {
			ch = styler.SafeGetCharAt(++pos);
		}
		if (ch == ')') {
			ch = styler.SafeGetCharAt(++pos);
			keyLen = pos - sc.currentPos;
		} else {
			return 0;
		}
	}
	// 3. (optional) Conversion flags
	while (AnyOf(ch, '#', '0', '-', ' ', '+')) {
		ch = styler.SafeGetCharAt(++pos);
	}
	// 4. (optional) Minimum field width
	if (ch == '*') {
		ch = styler.SafeGetCharAt(++pos);
	} else {
		while (IsADigit(ch)) {
			ch = styler.SafeGetCharAt(++pos);
		}
	}
	// 5. (optional) Precision
	if (ch == '.') {
		ch = styler.SafeGetCharAt(++pos);
		if (ch == '*') {
			ch = styler.SafeGetCharAt(++pos);
		} else {
			while (IsADigit(ch)) {
				ch = styler.SafeGetCharAt(++pos);
			}
		}
	}
	// 6. (optional) Length modifier
	if (ch == 'h' || ch == 'l' || ch == 'L') {
		ch = styler.SafeGetCharAt(++pos);
	}
	// 7. Conversion type
	if (IsPercentFormatSpecifier(ch)) {
		return pos - sc.currentPos + 1;
	}
	return 0;
}

// https://docs.python.org/3/library/string.html#formatspec

constexpr bool IsConversionChar(int ch) noexcept {
	return ch == 's' || ch == 'r' || ch == 'a';
}

constexpr bool IsBraceFormatSpecifier(char ch) noexcept {
	return AnyOf(ch, 'b',
					'c',
					'd',
					'e', 'E',
					'f', 'F',
					'g', 'G',
					'n',
					'o',
					's',
					'x', 'X',
					'%');
}

inline bool IsPyFormattedStringEnd(const StyleContext &sc, int braceCount) noexcept {
	return (sc.ch == '}' && braceCount == 0) // f"{ {1:2}[1] }"
		|| sc.ch == ':'
		|| (sc.ch == '!' && sc.chNext != '=')
		|| (sc.ch == '=' && !AnyOf(sc.chPrev, '<', '>', '=', '!'));
}

Sci_Position CheckBraceFormatSpecifier(const StyleContext &sc, LexAccessor &styler) noexcept {
	Sci_PositionU pos = sc.currentPos;
	// ["!" conversion]
	if (sc.ch == '!' && IsConversionChar(sc.chNext)) {
		pos += 2;
	}
	// [":" format_spec]
	char ch = styler.SafeGetCharAt(pos);
	if (ch != ':') {
		return pos - sc.currentPos;
	}

	ch = styler.SafeGetCharAt(++pos);
	if (ch == '%') {
		const char chNext = styler.SafeGetCharAt(pos + 1);
		if (IsDateTimeFormatSpecifier(chNext)) {
			return pos + 2 - sc.currentPos;
		}
	}
	// [[fill] align]
	if (!AnyOf(ch, '\r', '\n', '{', '}')) {
		Sci_Position width = 1;
		if (ch & 0x80) {
			styler.GetCharacterAndWidth(pos, &width);
		}
		const char chNext = styler.SafeGetCharAt(pos + width);
		if (AnyOf(ch, '<', '>', '=', '^') || AnyOf(chNext, '<', '>', '=', '^')) {
			pos += 1 + width;
			ch = styler.SafeGetCharAt(pos);
		}
	}
	// [sign][z][#]
	if (ch == '+' || ch == '-' || ch == ' ') {
		ch = styler.SafeGetCharAt(++pos);
	}
	if (ch == 'z') { // PEP 682
		ch = styler.SafeGetCharAt(++pos);
	}
	if (ch == '#') {
		ch = styler.SafeGetCharAt(++pos);
	}
	// [0][width]
	while (IsADigit(ch)) {
		ch = styler.SafeGetCharAt(++pos);
	}
	// [grouping_option]
	if (ch == '_' || ch == ',') {
		ch = styler.SafeGetCharAt(++pos);
	}
	// [.precision]
	if (ch == '.') {
		ch = styler.SafeGetCharAt(++pos);
		while (IsADigit(ch)) {
			ch = styler.SafeGetCharAt(++pos);
		}
	}
	// [type]
	if (IsBraceFormatSpecifier(ch)) {
		++pos;
	}
	return pos - sc.currentPos;
}

constexpr bool IsDocCommentTag(int state, int chNext) noexcept {
	return IsPyString(state) && IsPyTripleQuotedString(state) && (chNext == 'p' || chNext == 't' || chNext == 'r');
}

enum {
	PyLineStateMaskEmptyLine = 1 << 0,
	PyLineStateMaskCommentLine = 1 << 1,
	PyLineStateMaskTripleQuote = 1 << 2,
	PyLineStateMaskCloseBrace = 1 << 3,
	PyLineStateLineContinuation = 1 << 4,
	PyLineStateStringInterpolation = 1 << 5,
};

//KeywordIndex++Autogenerated -- start of section automatically generated
enum {
	KeywordIndex_Keyword = 0,
	KeywordIndex_Type = 1,
	KeywordIndex_BuiltinConstant = 2,
	KeywordIndex_BuiltinFunction = 3,
	KeywordIndex_Attribute = 4,
	KeywordIndex_SpecialMethod = 5,
	KeywordIndex_Class = 6,
};
//KeywordIndex--Autogenerated -- end of section automatically generated

void ColourisePyDoc(Sci_PositionU startPos, Sci_Position lengthDoc, int initStyle, LexerWordList keywordLists, Accessor &styler) {
	int kwType = SCE_PY_DEFAULT;
	int visibleChars = 0;
	int visibleCharsBefore = 0;
	int prevIndentCount = 0;
	int indentCount = 0;
	int parenCount = 0;
	int lineState = 0;
	bool prevLineContinuation = false;
	bool lineContinuation = false;
	bool insideUrl = false;
	bool insideFStringFormatSpec = false;
	EscapeSequence escSeq;

	std::vector<FormattedStringState> nestedState;

	if (startPos != 0) {
		// backtrack to the line starts expression inside formatted string literal.
		BacktrackToStart(styler, PyLineStateStringInterpolation, startPos, lengthDoc, initStyle);
	}

	StyleContext sc(startPos, lengthDoc, initStyle, styler);
	if (sc.currentLine > 0) {
		prevLineContinuation = (styler.GetLineState(sc.currentLine - 2) & PyLineStateLineContinuation) != 0;
		lineState = styler.GetLineState(sc.currentLine - 1);
		parenCount = (lineState >> 8) & 0xff;
		prevIndentCount = lineState >> 16;
		lineContinuation= (lineState & PyLineStateLineContinuation) != 0;
		lineState = 0;
	}

	while (sc.More()) {
		switch (sc.state) {
		case SCE_PY_OPERATOR:
		case SCE_PY_OPERATOR2:
			sc.SetState(SCE_PY_DEFAULT);
			break;

		case SCE_PY_NUMBER:
			if (!IsDecimalNumber(sc.chPrev, sc.ch, sc.chNext)) {
				sc.SetState(SCE_PY_DEFAULT);
			}
			break;

		case SCE_PY_IDENTIFIER:
			if (!IsIdentifierCharEx(sc.ch)) {
				char s[128];
				sc.GetCurrent(s, sizeof(s));
				if (keywordLists[KeywordIndex_Keyword]->InList(s)) {
					sc.ChangeState(SCE_PY_WORD);
					if (StrEqual(s, "def")) {
						kwType = SCE_PY_FUNCTION_DEFINITION;
					} else if (StrEqualsAny(s, "class", "raise", "except")) {
						kwType = SCE_PY_CLASS;
					}
				} else if (keywordLists[KeywordIndex_Type]->InList(s)) {
					sc.ChangeState(SCE_PY_WORD2);
				} else if (keywordLists[KeywordIndex_BuiltinConstant]->InList(s)) {
					sc.ChangeState(SCE_PY_BUILTIN_CONSTANT);
				} else if (keywordLists[KeywordIndex_BuiltinFunction]->InListPrefixed(s, '(')) {
					sc.ChangeState(SCE_PY_BUILTIN_FUNCTION);
				} else if (keywordLists[KeywordIndex_Attribute]->InList(s)) {
					sc.ChangeState(SCE_PY_ATTRIBUTE);
				} else if (keywordLists[KeywordIndex_SpecialMethod]->InListPrefixed(s, '(')) {
					sc.ChangeState(SCE_PY_OBJECT_FUNCTION);
				} else if (keywordLists[KeywordIndex_Class]->InList(s)) {
					sc.ChangeState(SCE_PY_CLASS);
				} else if (kwType != SCE_PY_DEFAULT) {
					sc.ChangeState(kwType);
				} else if (sc.GetLineNextChar() == '(') {
					sc.ChangeState(SCE_PY_FUNCTION);
				}
				if (sc.state != SCE_PY_WORD) {
					kwType = SCE_PY_DEFAULT;
				}
				sc.SetState(SCE_PY_DEFAULT);
			}
			break;

		case SCE_PY_DECORATOR:
			if (sc.ch == '.') {
				sc.SetState(SCE_PY_OPERATOR);
				sc.ForwardSetState(SCE_PY_DECORATOR);
			} else if (!IsIdentifierCharEx(sc.ch)) {
				sc.SetState(SCE_PY_DEFAULT);
			}
			break;

		case SCE_PY_STRING_SQ:
		case SCE_PY_STRING_DQ:
		case SCE_PY_TRIPLE_STRING_SQ:
		case SCE_PY_TRIPLE_STRING_DQ:
		case SCE_PY_RAWSTRING_SQ:
		case SCE_PY_RAWSTRING_DQ:
		case SCE_PY_TRIPLE_RAWSTRING_SQ:
		case SCE_PY_TRIPLE_RAWSTRING_DQ:

		case SCE_PY_FMTSTRING_SQ:
		case SCE_PY_FMTSTRING_DQ:
		case SCE_PY_TRIPLE_FMTSTRING_SQ:
		case SCE_PY_TRIPLE_FMTSTRING_DQ:
		case SCE_PY_RAWFMTSTRING_SQ:
		case SCE_PY_RAWFMTSTRING_DQ:
		case SCE_PY_TRIPLE_RAWFMTSTRING_SQ:
		case SCE_PY_TRIPLE_RAWFMTSTRING_DQ:

		case SCE_PY_BYTES_SQ:
		case SCE_PY_BYTES_DQ:
		case SCE_PY_TRIPLE_BYTES_SQ:
		case SCE_PY_TRIPLE_BYTES_DQ:
		case SCE_PY_RAWBYTES_SQ:
		case SCE_PY_RAWBYTES_DQ:
		case SCE_PY_TRIPLE_RAWBYTES_SQ:
		case SCE_PY_TRIPLE_RAWBYTES_DQ:
			if (sc.atLineStart && !lineContinuation) {
				if (!IsPyTripleQuotedString(sc.state)) {
					sc.SetState(SCE_PY_DEFAULT);
					break;
				}
			}
			if (insideUrl && IsInvalidUrlChar(sc.ch)) {
				insideUrl = false;
			}
			switch (sc.ch) {
			case '\\':
				if (IsEOLChar(sc.chNext)) {
					// nop
				} else if (IsPyRawString(sc.state)) {
					if (sc.chNext == '\\' || sc.chNext == '\'' || sc.chNext == '\"') {
						sc.Forward();
					}
				} else if (escSeq.resetEscapeState(sc.state, sc.chNext)) {
					sc.SetState(SCE_PY_ESCAPECHAR);
					sc.Forward();
				}
				break;

			case '\'':
			case '\"':
				if ((sc.ch == '\'') ^ IsPyDoubleQuotedString(sc.state)) {
					int offset = 0;
					if (IsPyTripleQuotedString(sc.state)) {
						if (sc.chNext == sc.ch && sc.GetRelative(2) == sc.ch) {
							offset = 3;
						}
					} else {
						offset = 1;
					}
					if (offset) {
						sc.Forward(offset);
						if (!nestedState.empty() && nestedState.back().state == sc.state) {
							nestedState.pop_back();
						}
						sc.SetState(SCE_PY_DEFAULT);
					}
				}
				break;

			case '{':
				if (IsPyString(sc.state)) {
					if (sc.chNext == '{') {
						escSeq.outerState = sc.state;
						escSeq.digitsLeft = 1;
						sc.SetState(SCE_PY_ESCAPECHAR);
						sc.Forward();
					} else if (IsPyFormattedString(sc.state)) {
						if (nestedState.empty()) {
							nestedState.push_back({sc.state, 1, 0});
						} else {
							nestedState.back().nestedLevel += 1;
						}
						insideFStringFormatSpec = false;
						sc.SetState(SCE_PY_OPERATOR2);
						sc.ForwardSetState(SCE_PY_DEFAULT);
					} else if (sc.chNext == '}' || sc.chNext == '!' || sc.chNext == ':' || IsIdentifierCharEx(sc.chNext)) {
						escSeq.outerState = sc.state;
						sc.SetState(SCE_PY_PLACEHOLDER);
					}
				}
				break;

			case '}':
				if (IsPyString(sc.state)) {
					if (IsPyFormattedString(sc.state)) {
						const bool interpolating = !nestedState.empty();
						if (interpolating) {
							FormattedStringState &state = nestedState.back();
							if (state.nestedLevel > 1) {
								--state.nestedLevel;
							} else {
								nestedState.pop_back();
							}
						}
						if (interpolating || sc.chNext != '}') {
							const int state = sc.state;
							sc.SetState(SCE_PY_OPERATOR2);
							sc.ForwardSetState(state);
							insideFStringFormatSpec = false;
							continue;
						}
					}
					if (sc.chNext == '}') {
						escSeq.outerState = sc.state;
						escSeq.digitsLeft = 1;
						sc.SetState(SCE_PY_ESCAPECHAR);
						sc.Forward();
					}
				}
				break;

			case '%': {
				Sci_Position keyLen = 0;
				Sci_Position length = CheckPercentFormatSpecifier(sc, styler, keyLen, insideUrl);
				if (length != 0) {
					const int state = sc.state;
					sc.SetState(SCE_PY_FORMAT_SPECIFIER);
					if (keyLen) {
						length -= keyLen;
						sc.ForwardSetState(SCE_PY_PLACEHOLDER);
						sc.Advance(keyLen - 1);
						sc.SetState(SCE_PY_FORMAT_SPECIFIER);
					}
					sc.Advance(length);
					sc.SetState(state);
					continue;
				}
			} break;

			case '$':
				// https://docs.python.org/3/library/string.html#template-strings
				if (IsPyString(sc.state)) {
					if (sc.chNext == '$') {
						escSeq.outerState = sc.state;
						escSeq.digitsLeft = 1;
						sc.SetState(SCE_PY_ESCAPECHAR);
						sc.Forward();
					} else if (sc.chNext == '{') {
						if (!IsPyFormattedString(sc.state) && IsIdentifierStart(sc.GetRelative(2))) {
							escSeq.outerState = sc.state;
							sc.SetState(SCE_PY_DOLLAR_PLACEHOLDER);
							sc.Advance(2);
						}
					} else if (IsIdentifierStart(sc.chNext)) {
						escSeq.outerState = sc.state;
						sc.SetState(SCE_PY_DOLLAR_PLACEHOLDER);
					}
				}
				break;

			case '!':
			case ':':
				if (insideFStringFormatSpec) {
					const Sci_Position length = CheckBraceFormatSpecifier(sc, styler);
					if (length != 0) {
						const int state = sc.state;
						sc.SetState(SCE_PY_FORMAT_SPECIFIER);
						sc.Advance(length);
						sc.SetState(state);
						continue;
					}
				}
				if (sc.ch == ':') {
					if (sc.MatchNext('/', '/') && IsLowerCase(sc.chPrev)) {
						insideUrl = true;
					} else if (visibleChars == 0 && IsDocCommentTag(sc.state, sc.chNext)) {
						escSeq.outerState = sc.state;
						sc.SetState(SCE_PY_COMMENTTAGAT);
					}
				}
				break;

			case '@':
				if (visibleChars == 0 && IsDocCommentTag(sc.state, sc.chNext)) {
					escSeq.outerState = sc.state;
					sc.SetState(SCE_PY_COMMENTTAGAT);
				}
				break;
			}
			break;

		case SCE_PY_ESCAPECHAR:
			if (escSeq.atEscapeEnd(sc.ch)) {
				sc.SetState(escSeq.outerState);
				continue;
			}
			break;

		case SCE_PY_PLACEHOLDER:
			// https://docs.python.org/3/library/string.html#format-string-syntax
			if (sc.ch == '.' && IsIdentifierCharEx(sc.chNext)) {
				sc.Forward();
			} else if (!IsIdentifierCharEx(sc.ch)) {
				bool match = true;
				if (sc.ch == '[') {
					if (IsADigit(sc.chNext)) {
						sc.Advance(2);
						while (IsADigit(sc.ch)) {
							sc.Forward();
						}
						if (sc.ch == ']') {
							sc.Forward();
						} else {
							match = false;
						}
					}
					// TODO: index_string
				}
				if (match && (sc.ch == '!' || sc.ch == ':')) {
					const Sci_Position length = CheckBraceFormatSpecifier(sc, styler);
					if (length != 0 && styler[sc.currentPos + length] == '}') {
						sc.SetState(SCE_PY_FORMAT_SPECIFIER);
						sc.Advance(length);
						sc.SetState(SCE_PY_PLACEHOLDER);
						sc.ForwardSetState(escSeq.outerState);
						continue;
					}
				}
				if (sc.ch != '}' || !match) {
					sc.Rewind();
					sc.ChangeState(escSeq.outerState);
				}
				sc.ForwardSetState(escSeq.outerState);
				continue;
			}
			break;

		case SCE_PY_DOLLAR_PLACEHOLDER:
			if (!IsIdentifierChar(sc.ch)) {
				if (sc.ch == '}' && styler[styler.GetStartSegment() + 1] == '{') {
					sc.Forward();
				}
				sc.SetState(escSeq.outerState);
				continue;
			}
			break;

		case SCE_PY_COMMENTTAGAT:
			if (!IsLowerCase(sc.ch)) {
				if (sc.ch == ' ' || sc.ch == ':') {
					if (sc.ch == ':') {
						sc.Forward();
					}
					sc.SetState(escSeq.outerState);
				} else {
					sc.ChangeState(escSeq.outerState);
				}
				continue;
			}
			break;

		case SCE_PY_COMMENTLINE:
			if (sc.atLineStart) {
				sc.SetState(SCE_PY_DEFAULT);
			} else {
				HighlightTaskMarker(sc, visibleChars, visibleCharsBefore, SCE_PY_TASKMARKER);
			}
			break;
		}

		if (sc.state == SCE_PY_DEFAULT) {
			if (sc.ch == '#') {
				visibleCharsBefore = visibleChars;
				if (visibleChars == 0) {
					lineState = PyLineStateMaskCommentLine;
				}
				sc.SetState(SCE_PY_COMMENTLINE);
			} else if (sc.ch == '\'') {
				insideUrl = false;
				if (sc.MatchNext('\'', '\'')) {
					sc.SetState(SCE_PY_TRIPLE_STRING_SQ);
					sc.Advance(2);
				} else {
					sc.SetState(SCE_PY_STRING_SQ);
				}
			} else if (sc.ch == '\"') {
				insideUrl = false;
				if (sc.MatchNext('\"', '\"')) {
					sc.SetState(SCE_PY_TRIPLE_STRING_DQ);
					sc.Advance(2);
				} else {
					sc.SetState(SCE_PY_STRING_DQ);
				}
			} else if (IsADigit(sc.ch) || (sc.ch == '.' && IsADigit(sc.chNext))) {
				sc.SetState(SCE_PY_NUMBER);
			} else if (IsPyStringPrefix(sc.ch)) {
				insideUrl = false;
				EnterPyStringState(sc);
			} else if (IsIdentifierStartEx(sc.ch)) {
				sc.SetState(SCE_PY_IDENTIFIER);
			} else if (sc.ch == '@') {
				if (!lineContinuation && visibleChars == 0 && parenCount == 0 && IsIdentifierStartEx(sc.chNext)) {
					sc.SetState(SCE_PY_DECORATOR);
				} else {
					sc.SetState(SCE_PY_OPERATOR);
				}
			} else if (isoperator(sc.ch)) {
				const bool interpolating = !nestedState.empty();
				int braceCount = 0;
				if (sc.ch == '{' || sc.ch == '[' || sc.ch == '(') {
					if (interpolating) {
						nestedState.back().parenCount += 1;
					} else {
						++parenCount;
					}
				} else if (sc.ch == '}' || sc.ch == ']' || sc.ch == ')') {
					if (interpolating) {
						FormattedStringState &state = nestedState.back();
						if (state.parenCount > 0) {
							braceCount = state.parenCount;
							--state.parenCount;
						}
					} else {
						if (parenCount > 0) {
							--parenCount;
						}
						if (visibleChars == 0) {
							lineState |= PyLineStateMaskCloseBrace;
						}
					}
				}
				if (interpolating) {
					const FormattedStringState &state = nestedState.back();
					if (state.parenCount == 0 && IsPyFormattedStringEnd(sc, braceCount)) {
						insideFStringFormatSpec = sc.ch != '}';
						sc.SetState(state.state);
						continue;
					}
				}
				sc.SetState(interpolating ? SCE_PY_OPERATOR2 : SCE_PY_OPERATOR);
				kwType = SCE_PY_DEFAULT;
			}
		}

		if (visibleChars == 0) {
			if (sc.ch == ' ') {
				++indentCount;
			} else if (sc.ch == '\t') {
				indentCount = GetTabIndentCount(indentCount);
			}
		}
		if (!isspacechar(sc.ch)) {
			visibleChars++;
		}
		if (sc.atLineEnd) {
			if (lineContinuation) {
				indentCount = prevIndentCount;
				if (!prevLineContinuation) {
					++indentCount;
				}
			}
			lineState |= (indentCount << 16) | (parenCount << 8);
			prevIndentCount = indentCount;
			prevLineContinuation = lineContinuation;
			if (sc.state != SCE_PY_COMMENTLINE && sc.LineEndsWith('\\')) {
				lineContinuation = true;
				lineState |= PyLineStateLineContinuation;
			} else {
				lineContinuation = false;
				if (!nestedState.empty()) {
					const int state = nestedState.back().state;
					if (!IsPyTripleQuotedString(state)) {
						nestedState.pop_back();
						sc.SetState(state);
					}
				}
			}
			if (!nestedState.empty()) {
				lineState |= PyLineStateStringInterpolation | PyLineStateMaskTripleQuote;
			} else if (IsPyStringStyle(sc.state) && IsPyTripleQuotedString(sc.state)) {
				lineState |= PyLineStateMaskTripleQuote;
			} else if (visibleChars == 0) {
				lineState |= PyLineStateMaskEmptyLine;
			}
			styler.SetLineState(sc.currentLine, lineState);
			lineState = 0;
			kwType = SCE_PY_DEFAULT;
			insideFStringFormatSpec = false;
			visibleChars = 0;
			visibleCharsBefore = 0;
			indentCount = 0;
		}
		sc.Forward();
	}

	sc.Complete();
}

constexpr bool IsCommentLine(int lineState) noexcept {
	return (lineState & PyLineStateMaskCommentLine) != 0;
}

struct FoldLineState {
	int lineState;
	int indentCount;
	constexpr explicit FoldLineState(int lineState_) noexcept : lineState(lineState_), indentCount(lineState_ >> 16) {}
	constexpr bool Empty() const noexcept {
		return (lineState & (PyLineStateMaskEmptyLine | PyLineStateMaskCommentLine)) != 0;
	}
	constexpr bool TripleQuoted() const noexcept {
		return (lineState & PyLineStateMaskTripleQuote) != 0;
	}
	constexpr bool Backtrack() const noexcept {
		return (lineState & (PyLineStateMaskEmptyLine | PyLineStateMaskCommentLine | PyLineStateMaskTripleQuote)) != 0;
	}
	constexpr bool CloseBrace() const noexcept {
		return (lineState & PyLineStateMaskCloseBrace) != 0;
	}
};

}

namespace Lexilla {

// code folding based on LexYAML
void FoldPyDoc(Sci_PositionU startPos, Sci_Position lengthDoc, int /*initStyle*/, LexerWordList /*keywordLists*/, Accessor &styler) {
	const Sci_Position maxPos = startPos + lengthDoc;
	const Sci_Line docLines = styler.GetLine(styler.Length());
	const Sci_Line maxLines = (maxPos == styler.Length()) ? docLines : styler.GetLine(maxPos - 1);

	Sci_Line lineCurrent = styler.GetLine(startPos);
	FoldLineState statePrev(styler.GetLineState(lineCurrent - 1));
	FoldLineState stateCurrent(styler.GetLineState(lineCurrent));
	while (lineCurrent > 0) {
		lineCurrent--;
		stateCurrent = statePrev;
		statePrev = FoldLineState(styler.GetLineState(lineCurrent - 1));
		if (!(stateCurrent.Backtrack() || statePrev.TripleQuoted())) {
			break;
		}
	}

	while (lineCurrent <= maxLines) {
		int lev = stateCurrent.indentCount + SC_FOLDLEVELBASE;
		if (stateCurrent.TripleQuoted()) {
			statePrev = stateCurrent;
			styler.SetLevel(lineCurrent, lev | SC_FOLDLEVELHEADERFLAG);
			int lineState;
			do {
				lineCurrent++;
				lineState = styler.GetLineState(lineCurrent);
				styler.SetLevel(lineCurrent, lev + 1);
			} while (lineState & PyLineStateMaskTripleQuote);
			lineCurrent++;
			stateCurrent = FoldLineState(styler.GetLineState(lineCurrent));
			if (stateCurrent.Empty()) {
				stateCurrent.indentCount = statePrev.indentCount;
			}
			continue;
		}

		Sci_Line lineNext = lineCurrent + 1;
		FoldLineState stateNext = stateCurrent;
		if (lineNext <= docLines) {
			stateNext = FoldLineState(styler.GetLineState(lineNext));
		}
		while ((lineNext < docLines) && stateNext.Empty()) {
			lineNext++;
			stateNext = FoldLineState(styler.GetLineState(lineNext));
		}

		int levelAfterBlank = stateNext.indentCount;
		if (!stateCurrent.Empty()) {
			if (stateNext.CloseBrace() && levelAfterBlank < stateCurrent.indentCount) {
				levelAfterBlank = stateCurrent.indentCount;
			}
			if ((stateCurrent.indentCount < levelAfterBlank)) {
				lev |= SC_FOLDLEVELHEADERFLAG;
			} else if (stateCurrent.CloseBrace() && stateCurrent.indentCount < statePrev.indentCount) {
				lev = statePrev.indentCount + SC_FOLDLEVELBASE;
			}
		}
		styler.SetLevel(lineCurrent, lev);
		lineCurrent++;

		if (lineCurrent < lineNext) {
			const int skipLevel = levelAfterBlank + SC_FOLDLEVELBASE;

			int prevLineState = stateCurrent.lineState;
			int nextLineState = styler.GetLineState(lineCurrent);
			int prevLevel = skipLevel;
			// comment on first line
			if (IsCommentLine(prevLineState)) {
				nextLineState = prevLineState;
				prevLineState = 0;
				--lineCurrent;
			}
			for (; lineCurrent < lineNext; lineCurrent++) {
				int level = skipLevel;
				const int currentLineState = nextLineState;
				nextLineState = styler.GetLineState(lineCurrent + 1);
				if (IsCommentLine(currentLineState)) {
					if (IsCommentLine(nextLineState) && !IsCommentLine(prevLineState)) {
						level |= SC_FOLDLEVELHEADERFLAG;
					} else if (prevLevel & SC_FOLDLEVELHEADERFLAG) {
						level++;
					} else {
						level = prevLevel;
					}
				}

				styler.SetLevel(lineCurrent, level);
				prevLineState = currentLineState;
				prevLevel = level;
			}
		}

		statePrev = stateCurrent;
		stateCurrent = stateNext;
	}
}

}

LexerModule lmPython(SCLEX_PYTHON, ColourisePyDoc, "python", FoldPyDoc);
