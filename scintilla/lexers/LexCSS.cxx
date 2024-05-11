// This file is part of Notepad2.
// See License.txt for details about distribution and modification.
//! Lexer for CSS.

#include <cassert>
#include <cstring>

#include <string>
#include <string_view>

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
#include "DocUtils.h"

using namespace Lexilla;

namespace {

// https://developer.mozilla.org/en-US/docs/Glossary/CSS_preprocessor
enum class Preprocessor {
	Standard,
	Scss,		// https://sass-lang.com/documentation
	Less,		// https://lesscss.org/features/
	HSS,		// https://github.com/ncannasse/hss
};

//KeywordIndex++Autogenerated -- start of section automatically generated
enum {
	KeywordIndex_Property = 0,
	KeywordIndex_AtRule = 1,
	KeywordIndex_PseudoClass = 2,
	KeywordIndex_PseudoElement = 3,
	KeywordIndex_MathFunction = 4,
};
//KeywordIndex--Autogenerated -- end of section automatically generated

struct EscapeSequence {
	int outerState = SCE_CSS_DEFAULT;
	int digitsLeft = 0;

	// highlight any character as escape sequence.
	void resetEscapeState(int state, int chNext) noexcept {
		outerState = state;
		digitsLeft = IsHexDigit(chNext) ? 6 : 1;
	}
	bool atEscapeEnd(int ch) noexcept {
		--digitsLeft;
		return digitsLeft <= 0 || !IsHexDigit(ch);
	}
	bool atUnicodeRangeEnd(int ch) noexcept {
		--digitsLeft;
		return digitsLeft <= 0 || !IsCssUnicodeRangeChar(ch);
	}
};

constexpr bool IsSpaceEquiv(int state) noexcept {
	return state <= SCE_CSS_CDO_CDC;
}

constexpr bool IsProperty(int style) noexcept {
	return style == SCE_CSS_PROPERTY || style == SCE_CSS_UNKNOWN_PROPERTY;
}

constexpr bool IsCssIdentifierStartEx(int ch, int chNext, Preprocessor preprocessor) noexcept {
	return IsIdentifierStartEx(ch)
		|| ((ch == '-' || ch == '@' || (preprocessor != Preprocessor::Standard && ch == '$'))
			&& IsCssIdentifierNext(chNext));
}

void ColouriseCssDoc(Sci_PositionU startPos, Sci_Position lengthDoc, int initStyle, LexerWordList keywordLists, Accessor &styler) {
	const Preprocessor preprocessor = static_cast<Preprocessor>(styler.GetPropertyInt("lexer.lang"));
	const bool fold = styler.GetPropertyBool("fold");
	bool propertyValue = false;
	bool attributeSelector = false;
	bool calcFunc = false;
	int variableInterpolation = 0;

	int parenCount = 0;		// function
	int calcLevel = 0;		// math function
	int selectorLevel = 0;	// nested selector
	int chBefore = 0;
	int chPrevNonWhite = 0;
	int stylePrevNonWhite = SCE_CSS_DEFAULT;
	int levelCurrent = SC_FOLDLEVELBASE;
	EscapeSequence escSeq;

	StyleContext sc(startPos, lengthDoc, initStyle, styler);
	if (sc.currentLine > 0) {
		levelCurrent = styler.LevelAt(sc.currentLine - 1) >> 16;
		const uint32_t lineState = styler.GetLineState(sc.currentLine - 1);
		/*
		1: propertyValue
		1: attributeSelector
		6: calcLevel
		8: parenCount
		8: selectorLevel
		*/
		propertyValue = lineState & true;
		attributeSelector = lineState & 2;
		calcLevel = (lineState >> 2) & 0x3f;
		parenCount = (lineState >> 8) & 0xff;
		selectorLevel = lineState >> 16;
	}
	if (startPos != 0 && IsSpaceEquiv(initStyle)) {
		LookbackNonWhite(styler, startPos, SCE_CSS_CDO_CDC, chPrevNonWhite, stylePrevNonWhite);
	}

	int levelNext = levelCurrent;
	while (sc.More()) {
		switch (sc.state) {
		case SCE_CSS_OPERATOR:
		case SCE_CSS_OPERATOR2:
		case SCE_CSS_CDO_CDC:
			sc.SetState(SCE_CSS_DEFAULT);
			break;

		case SCE_CSS_NUMBER:
			if (!IsDecimalNumber(sc.chPrev, sc.ch, sc.chNext)) {
				if (IsCssIdentifierStart(sc.ch, sc.chNext)) {
					sc.ChangeState(SCE_CSS_DIMENSION);
				} else {
					if (sc.ch == '%') {
						sc.Forward();
					}
					sc.SetState(SCE_CSS_DEFAULT);
				}
			}
			break;

		case SCE_CSS_COMMENTBLOCK:
		case SCE_CSS_COMMENTBLOCKDOC:
			if (sc.Match('*', '/')) {
				levelNext--;
				sc.Forward();
				sc.ForwardSetState(SCE_CSS_DEFAULT);
			}
			break;

		case SCE_CSS_COMMENTLINE:
		case SCE_CSS_COMMENTLINEDOC:
			if (sc.atLineStart) {
				sc.SetState(SCE_CSS_DEFAULT);
			}
			break;

		case SCE_CSS_DIMENSION:
		case SCE_CSS_VARIABLE:
		case SCE_CSS_AT_RULE:
		case SCE_CSS_IDENTIFIER:
		case SCE_CSS_PSEUDOCLASS:
		case SCE_CSS_PSEUDOELEMENT:
			if (!IsCssIdentifierChar(sc.ch)) {
				if (sc.state >= SCE_CSS_IDENTIFIER || (sc.state == SCE_CSS_AT_RULE && preprocessor == Preprocessor::Less)) {
					char s[128];
					sc.GetCurrentLowered(s, sizeof(s));
					switch (sc.state) {
					case SCE_CSS_IDENTIFIER: {
						const int chNext = sc.GetDocNextChar(sc.ch == '(');
						if (sc.ch == '(') {
							sc.ChangeState(SCE_CSS_FUNCTION);
							if (keywordLists[KeywordIndex_MathFunction].InListPrefixed(s, '(')) {
								calcFunc = true;
							} else if (StrEqualsAny(s, "url", "url-prefix")
								&& !AnyOf(chNext, '\'', '\"', ')') && (chNext != '$' || preprocessor != Preprocessor::Scss)) {
								levelNext++;
								parenCount++;
								sc.SetState(SCE_CSS_OPERATOR);
								sc.ForwardSetState(SCE_CSS_URL);
								continue;
							}
						} else if (chBefore == '!' && StrEqual(s, "important")) {
							sc.ChangeState(SCE_CSS_IMPORTANT);
						} else if (variableInterpolation) {
							if (preprocessor == Preprocessor::Less && chBefore == '{') {
								sc.ChangeState(SCE_CSS_VARIABLE);
							}
						} else if (chNext == ':' && parenCount != 0) {
							// (descriptor: value)
							sc.ChangeState(SCE_CSS_PROPERTY);
						} else if (chBefore == ':' || chBefore == '=' || (parenCount == 0 && propertyValue)) {
							// [attribute = value]
							sc.ChangeState(SCE_CSS_VALUE);
						} else if (!propertyValue) {
							if (attributeSelector) {
								sc.ChangeState(SCE_CSS_ATTRIBUTE);
							} else if (chBefore == '.') {
								sc.ChangeState(SCE_CSS_CLASS);
							} else if (chBefore == '#') {
								sc.ChangeState(SCE_CSS_ID);
							} else if (chBefore == '%' && preprocessor == Preprocessor::Scss) {
								sc.ChangeState(SCE_CSS_PLACEHOLDER);
							} else if (chNext == ':' && (chBefore == ';' || chBefore == '{')) {
								// {property: value;}
								propertyValue = true;
								if (keywordLists[KeywordIndex_Property].InList(s)) {
									sc.ChangeState(SCE_CSS_PROPERTY);
								} else {
									sc.ChangeState(SCE_CSS_UNKNOWN_PROPERTY);
								}
							} else if (parenCount == selectorLevel && !(chNext == '(')) {
								sc.ChangeState(SCE_CSS_TAG);
							}
						}
					} break;

					case SCE_CSS_AT_RULE:
						if (propertyValue || !keywordLists[KeywordIndex_AtRule].InList(s + 1)) {
							sc.ChangeState(SCE_CSS_VARIABLE);
						}
						break;

					case SCE_CSS_PSEUDOCLASS:
						if (!keywordLists[KeywordIndex_PseudoClass].InListPrefixed(s + 1, '(')) {
							sc.ChangeState(SCE_CSS_UNKNOWN_PSEUDOCLASS);
						} else if (sc.ch == '(') {
							if (StrEqualsAny(s + 1, "is", "has", "not", "where", "current")) {
								++selectorLevel;
							}
						}
						break;

					case SCE_CSS_PSEUDOELEMENT:
						if (!keywordLists[KeywordIndex_PseudoElement].InListPrefixed(s + 2, '(')) {
							sc.ChangeState(SCE_CSS_UNKNOWN_PSEUDOELEMENT);
						}
						break;
					}
				}

				stylePrevNonWhite = sc.state;
				sc.SetState(SCE_CSS_DEFAULT);
			}
			break;

		case SCE_CSS_STRING_SQ:
		case SCE_CSS_STRING_DQ:
		case SCE_CSS_URL:
			if (sc.ch == '\\') {
				if (!IsEOLChar(sc.chNext)) {
					escSeq.resetEscapeState(sc.state, sc.chNext);
					sc.SetState(SCE_CSS_ESCAPECHAR);
					sc.Forward();
				}
			} else if (sc.ch == ')' && sc.state == SCE_CSS_URL) {
				sc.SetState(SCE_CSS_DEFAULT);
			} else if ((sc.ch == '\'' && sc.state == SCE_CSS_STRING_SQ)
				|| (sc.ch == '\"' && sc.state == SCE_CSS_STRING_DQ)) {
				sc.ForwardSetState(SCE_CSS_DEFAULT);
			} else if (sc.chNext == '{' && ((preprocessor == Preprocessor::Scss && sc.ch == '#')
				|| (preprocessor == Preprocessor::Less && sc.ch == '@'))) {
				variableInterpolation = sc.state + 1;
				levelNext++;
				sc.SetState(SCE_CSS_OPERATOR);
				sc.Forward();
			}
			break;

		case SCE_CSS_ESCAPECHAR:
			if (escSeq.atEscapeEnd(sc.ch)) {
				sc.SetState(escSeq.outerState);
				continue;
			}
			break;

		case SCE_CSS_UNICODE_RANGE:
			if (sc.ch == '-' && IsCssUnicodeRangeChar(sc.chNext)) {
				escSeq.digitsLeft = 7;
			} else if (escSeq.atUnicodeRangeEnd(sc.ch)) {
				sc.SetState(SCE_CSS_DEFAULT);
			}
			break;
		}

		if (sc.state == SCE_CSS_DEFAULT) {
			if (sc.ch == '/' && (sc.chNext == '*' || sc.chNext == '/')) {
				const bool block = sc.chNext == '*';
				levelNext += block;
				sc.SetState(block ? SCE_CSS_COMMENTBLOCK : SCE_CSS_COMMENTLINE);
				sc.Forward();
				if (sc.chNext == '!' || (sc.ch == sc.chNext)) {
					sc.ChangeState(block ? SCE_CSS_COMMENTBLOCKDOC : SCE_CSS_COMMENTLINEDOC);
				}
			} else if (sc.ch == '\'') {
				sc.SetState(SCE_CSS_STRING_SQ);
			} else if (sc.ch == '\"') {
				sc.SetState(SCE_CSS_STRING_DQ);
			} else if (IsHtmlCommentDelimiter(sc)) {
				sc.SetState(SCE_CSS_CDO_CDC);
				sc.Advance((sc.ch == '<') ? 3 : 2);
			} else if (IsNumberStart(sc.ch, sc.chNext)
				|| (sc.ch == '#' && (propertyValue || parenCount > selectorLevel) && IsHexDigit(sc.chNext))) {
				sc.SetState(SCE_CSS_NUMBER);
			} else if (sc.chNext == '+' && UnsafeLower(sc.ch) == 'u'
				&& propertyValue && (chPrevNonWhite == ':' || chPrevNonWhite == ',')
				&& IsCssUnicodeRangeChar(sc.GetRelative(2))) {
				escSeq.digitsLeft = 7;
				sc.SetState(SCE_CSS_UNICODE_RANGE);
				sc.Forward();
			} else if (IsCssIdentifierStartEx(sc.ch, sc.chNext, preprocessor)) {
				chBefore = chPrevNonWhite;
				sc.SetState((sc.ch == '@') ? SCE_CSS_AT_RULE : ((sc.ch == '$') ? SCE_CSS_VARIABLE : SCE_CSS_IDENTIFIER));
			} else if (sc.Match(':', ':') && IsCssIdentifierNext(sc.GetRelative(2))) {
				sc.SetState(SCE_CSS_PSEUDOELEMENT);
				sc.Forward(2);
			} else if (sc.ch == ':' && !IsProperty(stylePrevNonWhite)
				&& IsCssIdentifierNext(sc.chNext)) {
				sc.SetState(SCE_CSS_PSEUDOCLASS);
				sc.Forward();
			} else if (IsAGraphic(sc.ch)) {
				sc.SetState(SCE_CSS_OPERATOR);
				switch (sc.ch) {
				case '{':
					levelNext++;
					if ((preprocessor == Preprocessor::Scss && sc.chPrev == '#')
						|| (preprocessor == Preprocessor::Less && sc.chPrev == '@')) {
						variableInterpolation = SCE_CSS_DEFAULT + 1;
					} else {
						propertyValue = false;
						attributeSelector = false;
						parenCount = 0;
						calcLevel = 0;
						selectorLevel = 0;
					}
					break;
				case '}':
					levelNext--;
					if (variableInterpolation) {
						sc.ForwardSetState(variableInterpolation - 1);
						variableInterpolation = 0;
						continue;
					}
					propertyValue = false;
					attributeSelector = false;
					parenCount = 0;
					calcLevel = 0;
					selectorLevel = 0;
					break;
				case '[':
					levelNext++;
					attributeSelector = true;
					break;
				case ']':
					levelNext--;
					attributeSelector = false;
					break;
				case '(':
					levelNext++;
					parenCount++;
					if (calcLevel != 0 || calcFunc) {
						calcFunc = false;
						++calcLevel;
					}
					break;
				case ')':
					levelNext--;
					if (parenCount > 0) {
						parenCount--;
					}
					if (calcLevel > 0) {
						--calcLevel;
					}
					if (selectorLevel > 0) {
						selectorLevel--;
					}
					break;
				case ':':
					if (parenCount == 0 && !IsProperty(stylePrevNonWhite)) {
						propertyValue = true;
					}
					break;
				case ';':
					if (parenCount == 0 && !attributeSelector) {
						propertyValue = false;
					}
					break;
				case '+':
				case '-':
				case '*':
				case '/':
					if (calcLevel != 0 && (chPrevNonWhite == ')' || AnyOf(stylePrevNonWhite, SCE_CSS_NUMBER, SCE_CSS_DIMENSION))) {
						sc.ChangeState(SCE_CSS_OPERATOR2); // operator inside math function
					}
					break;
				}
			}
		}

		if (!IsSpaceEquiv(sc.state)) {
			chPrevNonWhite = sc.ch;
			stylePrevNonWhite = sc.state;
		}
		if (sc.atLineEnd) {
			if (fold) {
				levelNext = sci::max(levelNext, SC_FOLDLEVELBASE);
				const int levelUse = levelCurrent;
				int lev = levelUse | levelNext << 16;
				if (levelUse < levelNext) {
					lev |= SC_FOLDLEVELHEADERFLAG;
				}
				styler.SetLevel(sc.currentLine, lev);
			}

			const int lineState = static_cast<int>(propertyValue) | (static_cast<int>(attributeSelector) << 1)
				| (calcLevel << 2) | (parenCount << 8) | (selectorLevel << 16);
			styler.SetLineState(sc.currentLine, lineState);
			levelCurrent = levelNext;
		}
		sc.Forward();
	}

	sc.Complete();
}

}

LexerModule lmCSS(SCLEX_CSS, ColouriseCssDoc, "css");
