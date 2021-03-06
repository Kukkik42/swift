//===--- OldRemangler.cpp - Old Swift Re-mangler --------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2017 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
//  This file implements the remangler, which turns a demangling parse
//  tree back into a mangled string.  This is useful for tools which
//  want to extract subtrees from mangled strings.
//
//===----------------------------------------------------------------------===//

#include "swift/Demangling/Demangler.h"
#include "swift/Demangling/Punycode.h"
#include "swift/Strings.h"
#include <vector>
#include <cstdio>
#include <cstdlib>
#include <unordered_map>

using namespace swift;
using namespace Demangle;

[[noreturn]]
static void unreachable(const char *Message) {
  fprintf(stderr, "fatal error: %s\n", Message);
  std::abort();
}

/// Translate the given operator character into its mangled form.
///
/// Current operator characters:   @/=-+*%<>!&|^~ and the special operator '..'
static char mangleOperatorChar(char op) {
  switch (op) {
  case '&': return 'a'; // 'and'
  case '@': return 'c'; // 'commercial at sign'
  case '/': return 'd'; // 'divide'
  case '=': return 'e'; // 'equal'
  case '>': return 'g'; // 'greater'
  case '<': return 'l'; // 'less'
  case '*': return 'm'; // 'multiply'
  case '!': return 'n'; // 'negate'
  case '|': return 'o'; // 'or'
  case '+': return 'p'; // 'plus'
  case '?': return 'q'; // 'question'
  case '%': return 'r'; // 'remainder'
  case '-': return 's'; // 'subtract'
  case '~': return 't'; // 'tilde'
  case '^': return 'x'; // 'xor'
  case '.': return 'z'; // 'zperiod' (the z is silent)
  default:
    return op;
  }
}

static bool isNonAscii(StringRef str) {
  for (unsigned char c : str) {
    if (c >= 0x80)
      return true;
  }
  return false;
}

static char mangleOperatorKind(OperatorKind operatorKind) {
  switch (operatorKind) {
  case OperatorKind::NotOperator: unreachable("invalid");
  case OperatorKind::Infix: return 'i';
  case OperatorKind::Prefix: return 'p';
  case OperatorKind::Postfix: return 'P';
  }
  unreachable("invalid");
}

static void mangleIdentifier(StringRef ident, OperatorKind operatorKind,
                             bool usePunycode, DemanglerPrinter &out) {
  std::string punycodeBuf;
  if (usePunycode) {
    // If the identifier contains non-ASCII character, we mangle 
    // with an initial X and Punycode the identifier string.
    if (isNonAscii(ident)) {
      out << 'X';
      Punycode::encodePunycodeUTF8(ident, punycodeBuf);
      ident = punycodeBuf;
    }
  }

  // Mangle normal identifiers as
  //   count identifier-char+
  // where the count is the number of characters in the identifier,
  // and where individual identifier characters represent themselves.
  if (operatorKind == OperatorKind::NotOperator) {
    out << ident.size() << ident;
    return;
  }

  // Mangle operator identifiers as
  //   operator ::= 'o' operator-fixity count operator-char+
  //   operator-fixity ::= 'p' // prefix
  //   operator-fixity ::= 'P' // postfix
  //   operator-fixity ::= 'i' // infix
  // where the count is the number of characters in the operator,
  // and where the individual operator characters are translated.
  out << 'o' << mangleOperatorKind(operatorKind);

  // Mangle ASCII operators directly.
  out << ident.size();
  for (char ch : ident) {
    out << mangleOperatorChar(ch);
  }
}

void Demangle::mangleIdentifier(const char *data, size_t length,
                                OperatorKind operatorKind,
                                std::string &out, bool usePunycode) {
  DemanglerPrinter printer;
  ::mangleIdentifier(StringRef(data, length), operatorKind,
                     usePunycode, printer);
  out = std::move(printer).str();
}

namespace {
  struct DeepHasher {
    size_t value = 0;

    void combine(size_t newValue) {
      value = 33 * value + newValue;
    }

    void hash(Node *node) {
      combine((size_t) node->getKind());
      if (node->hasIndex()) {
        combine(node->getIndex());
      } else if (node->hasText()) {
        StringRef text = node->getText();
        for (char c : text) {
          combine((unsigned char) c);
        }
      }
      for (const auto &child : *node) {
        hash(child);
      }
    }
  };
} // end anonymous namespace

static size_t deepHash(Node *node) {
  DeepHasher hasher;
  hasher.hash(node);
  return hasher.value;  
}

static bool deepEquals(Node *lhs, Node *rhs) {
  if (lhs->getKind() != rhs->getKind())
    return false;
  if (lhs->hasIndex()) {
    if (!rhs->hasIndex())
      return false;
    if (lhs->getIndex() != rhs->getIndex())
      return false;
  } else if (lhs->hasText()) {
    if (!rhs->hasText())
      return false;
    if (lhs->getText() != rhs->getText())
      return false;
  } else if (rhs->hasIndex() || rhs->hasText()) {
    return false;
  }

  if (lhs->getNumChildren() != rhs->getNumChildren())
    return false;

  for (auto li = lhs->begin(), ri = lhs->begin(), le = lhs->end();
       li != le; ++li, ++ri) {
    if (!deepEquals(*li, *ri))
      return false;
  }

  return true;
}

namespace {
  struct SubstitutionEntry {
    Node *TheNode;
    size_t StoredHash;

    // Note that the constructor leaves this uninitialized.

    struct Hasher {
      size_t operator()(const SubstitutionEntry &entry) const {
        return entry.StoredHash;
      }
    };
    friend bool operator==(const SubstitutionEntry &lhs,
                           const SubstitutionEntry &rhs) {
      return (lhs.StoredHash == rhs.StoredHash &&
              deepEquals(lhs.TheNode, lhs.TheNode));
    }
  };

  class Remangler {
    DemanglerPrinter &Out;

    // We have to cons up temporary nodes sometimes when remangling
    // nested generics. This factory owns them.
    NodeFactory Factory;

    std::unordered_map<SubstitutionEntry, unsigned,
                       SubstitutionEntry::Hasher> Substitutions;
  public:
    Remangler(DemanglerPrinter &out) : Out(out) {}

    class EntityContext {
      bool AsContext = false;
    public:
      bool isAsContext() const {
        return AsContext;
      }

      class ManglingContextRAII {
        EntityContext &Ctx;
        bool SavedValue;
      public:
        ManglingContextRAII(EntityContext &ctx)
          : Ctx(ctx), SavedValue(ctx.AsContext) {
          ctx.AsContext = true;
        }

        ~ManglingContextRAII() {
          Ctx.AsContext = SavedValue;
        }
      };
    };

    void mangle(Node *node) {
      switch (node->getKind()) {
#define NODE(ID) case Node::Kind::ID: return mangle##ID(node);
#include "swift/Demangling/DemangleNodes.def"
      }
      unreachable("bad demangling tree node");
    }

    void mangleGenericArgs(Node *node, EntityContext &ctx);
    void mangleAnyNominalType(Node *node, EntityContext &ctx);

#define NODE(ID)                                                        \
    void mangle##ID(Node *node);
#define CONTEXT_NODE(ID)                                                \
    void mangle##ID(Node *node);                                        \
    void mangle##ID(Node *node, EntityContext &ctx);
#include "swift/Demangling/DemangleNodes.def"

    void mangleIndex(Node::IndexType index);
    void mangleIdentifier(StringRef name, OperatorKind operatorKind);
    void mangleAccessor(Node *storageNode, StringRef accessorCode,
                        EntityContext &ctx);

    void mangleChildNodes(Node *node) { mangleNodes(node->begin(), node->end()); }
    void mangleNodes(Node::iterator i, Node::iterator e) {
      for (; i != e; ++i) {
        mangle(*i);
      }
    }
    void mangleSingleChildNode(Node *node) {
      assert(node->getNumChildren() == 1);
      mangle(*node->begin());
    }
    void mangleChildNode(Node *node, unsigned index) {
      assert(index < node->getNumChildren());
      mangle(node->begin()[index]);
    }

    void mangleSimpleEntity(Node *node, char basicKind, StringRef entityKind,
                            EntityContext &ctx);
    void mangleNamedEntity(Node *node, char basicKind, StringRef entityKind,
                           EntityContext &ctx);
    void mangleTypedEntity(Node *node, char basicKind, StringRef entityKind,
                           EntityContext &ctx);
    void mangleNamedAndTypedEntity(Node *node, char basicKind,
                                   StringRef entityKind,
                                   EntityContext &ctx);
    void mangleNominalType(Node *node, char basicKind, EntityContext &ctx);

    void mangleProtocolWithoutPrefix(Node *node);
    void mangleProtocolListWithoutPrefix(Node *node,
                                         Node *additionalProto = nullptr);

    void mangleEntityContext(Node *node, EntityContext &ctx);
    void mangleEntityType(Node *node, EntityContext &ctx);
    void mangleEntityGenericType(Node *node, EntityContext &ctx);

    bool trySubstitution(Node *node, SubstitutionEntry &entry);
    bool mangleStandardSubstitution(Node *node);
    void addSubstitution(const SubstitutionEntry &entry);
    void resetSubstitutions();

    void mangleDependentGenericParamIndex(Node *node);
    void mangleConstrainedType(Node *node);
  };
} // end anonymous namespace

#define NODE(ID)
#define CONTEXT_NODE(ID)                        \
void Remangler::mangle##ID(Node *node) {        \
  EntityContext ctx;                            \
  mangle##ID(node, ctx);                        \
}
#include "swift/Demangling/DemangleNodes.def"

/// Reset the currently-active set of substitutions.  This is useful
/// when part of the mangling is done independently, e.g. when an
/// optimization pass modifies a pass.
void Remangler::resetSubstitutions() {
  Substitutions.clear();
}

bool Remangler::trySubstitution(Node *node, SubstitutionEntry &entry) {
  if (mangleStandardSubstitution(node))
    return true;

  // Go ahead and initialize the substitution entry.
  entry.TheNode = node;
  entry.StoredHash = deepHash(node);

  auto it = Substitutions.find(entry);
  if (it == Substitutions.end())
    return false;

  Out << 'S';
  mangleIndex(it->second);
  return true;
}

static bool isInSwiftModule(Node *node) {
  Node *context = node->getFirstChild();
  return (context->getKind() == Node::Kind::Module &&
          context->getText() == STDLIB_NAME);
};

bool Remangler::mangleStandardSubstitution(Node *node) {
  // Look for known substitutions.
  switch (node->getKind()) {
#define SUCCESS_IF_IS(VALUE, EXPECTED, SUBSTITUTION)            \
    do {                                                        \
      if ((VALUE) == (EXPECTED)) {                              \
        Out << SUBSTITUTION;                                    \
        return true;                                            \
      }                                                         \
    } while (0)
#define SUCCESS_IF_TEXT_IS(EXPECTED, SUBSTITUTION)              \
    SUCCESS_IF_IS(node->getText(), EXPECTED, SUBSTITUTION)
#define SUCCESS_IF_DECLNAME_IS(EXPECTED, SUBSTITUTION)          \
    SUCCESS_IF_IS(node->getChild(1)->getText(), EXPECTED, SUBSTITUTION)

    case Node::Kind::Module:
      SUCCESS_IF_TEXT_IS(STDLIB_NAME, "s");
      SUCCESS_IF_TEXT_IS(MANGLING_MODULE_OBJC, "So");
      SUCCESS_IF_TEXT_IS(MANGLING_MODULE_C, "SC");
      break;
    case Node::Kind::Structure:
      if (isInSwiftModule(node)) {
        SUCCESS_IF_DECLNAME_IS("Array", "Sa");
        SUCCESS_IF_DECLNAME_IS("Bool", "Sb");
        SUCCESS_IF_DECLNAME_IS("UnicodeScalar", "Sc");
        SUCCESS_IF_DECLNAME_IS("Double", "Sd");
        SUCCESS_IF_DECLNAME_IS("Float", "Sf");
        SUCCESS_IF_DECLNAME_IS("Int", "Si");
        SUCCESS_IF_DECLNAME_IS("UnsafeRawPointer", "SV");
        SUCCESS_IF_DECLNAME_IS("UnsafeMutableRawPointer", "Sv");
        SUCCESS_IF_DECLNAME_IS("UnsafePointer", "SP");
        SUCCESS_IF_DECLNAME_IS("UnsafeMutablePointer", "Sp");
        SUCCESS_IF_DECLNAME_IS("UnsafeBufferPointer", "SR");
        SUCCESS_IF_DECLNAME_IS("UnsafeMutableBufferPointer", "Sr");
        SUCCESS_IF_DECLNAME_IS("String", "SS");
        SUCCESS_IF_DECLNAME_IS("UInt", "Su");
      }
      break;
    case Node::Kind::Enum:
      if (isInSwiftModule(node)) {
        SUCCESS_IF_DECLNAME_IS("Optional", "Sq");
        SUCCESS_IF_DECLNAME_IS("ImplicitlyUnwrappedOptional", "SQ");
      }
      break;

    default:
      break;

#undef SUCCESS_IF_DECLNAME_IS
#undef SUCCESS_IF_TEXT_IS
#undef SUCCESS_IF_IS
  }
  return false;
}

void Remangler::addSubstitution(const SubstitutionEntry &entry) {
  auto result = Substitutions.insert({entry, Substitutions.size()});
  assert(result.second);
  (void) result;
}

void Remangler::mangleIdentifier(Node *node) {
  mangleIdentifier(node->getText(), OperatorKind::NotOperator);
}
void Remangler::manglePrefixOperator(Node *node) {
  mangleIdentifier(node->getText(), OperatorKind::Prefix);
}
void Remangler::manglePostfixOperator(Node *node) {
  mangleIdentifier(node->getText(), OperatorKind::Postfix);
}
void Remangler::mangleInfixOperator(Node *node) {
  mangleIdentifier(node->getText(), OperatorKind::Infix);
}
void Remangler::mangleIdentifier(StringRef ident, OperatorKind operatorKind) {
  ::mangleIdentifier(ident, operatorKind, /*usePunycode*/ false, Out);
}

void Remangler::mangleNumber(Node *node) {
  mangleIndex(node->getIndex());
}
void Remangler::mangleIndex(Node::IndexType value) {
  if (value == 0) {
    Out << '_';
  } else {
    Out << (value - 1) << '_';
  }
}

void Remangler::mangleGlobal(Node *node) {
  Out << "_T";
  mangleChildNodes(node);
}

void Remangler::mangleSuffix(Node *node) {
  // Just add the suffix back on.
  Out << node->getText();
}

void Remangler::mangleGenericSpecialization(Node *node) {
  Out << "TSg";
  mangleChildNodes(node); // GenericSpecializationParams

  // Specializations are just prepended to already-mangled names.
  resetSubstitutions();

  // Start another mangled name.
  Out << "__T";
}
void Remangler::mangleGenericSpecializationNotReAbstracted(Node *node) {
  Out << "TSr";
  mangleChildNodes(node); // GenericSpecializationParams

  // Specializations are just prepended to already-mangled names.
  resetSubstitutions();

  // Start another mangled name.
  Out << "__T";
}

void Remangler::mangleGenericPartialSpecialization(Node *node) {
  unreachable("todo");
}

void Remangler::mangleGenericPartialSpecializationNotReAbstracted(Node *node) {
  unreachable("todo");
}

void Remangler::mangleGenericSpecializationParam(Node *node) {
  // Should be a type followed by a series of protocol conformances.
  mangleChildNodes(node);
  Out << '_';
}

void Remangler::mangleFunctionSignatureSpecialization(Node *node) {
  Out << "TSf";
  mangleChildNodes(node); // FunctionSignatureSpecializationParams

  // Specializations are just prepended to already-mangled names.
  resetSubstitutions();

  // Start another mangled name.
  Out << "__T";
}

void Remangler::mangleSpecializationPassID(Node *node) {
  Out << node->getIndex();
}

void Remangler::mangleSpecializationIsFragile(Node *node) {
  Out << "q";
}

void Remangler::mangleFunctionSignatureSpecializationParam(Node *node) {
  if (!node->hasChildren()) {
    Out << "n_";
    return;
  }

  // The first child is always a kind that specifies the type of param that we
  // have.
  NodePointer firstChild = node->getChild(0);
  unsigned kindValue = firstChild->getIndex();
  auto kind = FunctionSigSpecializationParamKind(kindValue);

  switch (kind) {
  case FunctionSigSpecializationParamKind::ConstantPropFunction:
    Out << "cpfr";
    mangleIdentifier(node->getChild(1));
    Out << '_';
    return;
  case FunctionSigSpecializationParamKind::ConstantPropGlobal:
    Out << "cpg";
    mangleIdentifier(node->getChild(1));
    Out << '_';
    return;
  case FunctionSigSpecializationParamKind::ConstantPropInteger:
    Out << "cpi" << node->getChild(1)->getText() << '_';
    return;
  case FunctionSigSpecializationParamKind::ConstantPropFloat:
    Out << "cpfl" << node->getChild(1)->getText() << '_';
    return;
  case FunctionSigSpecializationParamKind::ConstantPropString: {
    Out << "cpse";
    StringRef encodingStr = node->getChild(1)->getText();
    if (encodingStr == "u8")
      Out << '0';
    else if (encodingStr == "u16")
      Out << '1';
    else
      unreachable("Unknown encoding");
    Out << 'v';
    mangleIdentifier(node->getChild(2));
    Out << '_';
    return;
  }
  case FunctionSigSpecializationParamKind::ClosureProp:
    Out << "cl";
    mangleIdentifier(node->getChild(1));
    for (unsigned i = 2, e = node->getNumChildren(); i != e; ++i) {
      mangleType(node->getChild(i));
    }
    Out << '_';
    return;
  case FunctionSigSpecializationParamKind::BoxToValue:
    Out << "i_";
    return;
  case FunctionSigSpecializationParamKind::BoxToStack:
    Out << "k_";
    return;
  default:
    if (kindValue &
        unsigned(FunctionSigSpecializationParamKind::Dead))
      Out << 'd';
    if (kindValue &
        unsigned(FunctionSigSpecializationParamKind::OwnedToGuaranteed))
      Out << 'g';
    if (kindValue & unsigned(FunctionSigSpecializationParamKind::SROA))
      Out << 's';
    Out << '_';
    return;
  }
}

void Remangler::mangleFunctionSignatureSpecializationParamPayload(Node *node) {
  // This should never be called since mangling parameter payloads require
  // knowing what the parameter kind is.
  unreachable("This should never be called");
}

void Remangler::mangleFunctionSignatureSpecializationParamKind(Node *node) {
  // This should never be called since mangling parameter kinds have influence
  // on the payloads.
  unreachable("This should never be called");
}

void Remangler::mangleProtocolConformance(Node *node) {
  // type, protocol name, context
  assert(node->getNumChildren() == 3);
  mangleChildNode(node, 0);
  mangleProtocolWithoutPrefix(node->begin()[1]);
  mangleChildNode(node, 2);
}

void Remangler::mangleObjCAttribute(Node *node) {
  Out << "To";
}

void Remangler::mangleNonObjCAttribute(Node *node) {
  Out << "TO";
}

void Remangler::mangleDirectMethodReferenceAttribute(Node *node) {
  Out << "Td";
}

void Remangler::mangleDynamicAttribute(Node *node) {
  Out << "TD";
}

void Remangler::mangleVTableAttribute(Node *node) {
  Out << "TV";
}

void Remangler::mangleGenericTypeMetadataPattern(Node *node) {
  Out << "MP";
  mangleSingleChildNode(node); // type
}

void Remangler::mangleTypeMetadataAccessFunction(Node *node) {
  Out << "Ma";
  mangleSingleChildNode(node); // type
}

void Remangler::mangleTypeMetadataLazyCache(Node *node) {
  Out << "ML";
  mangleSingleChildNode(node); // type
}

void Remangler::mangleMetaclass(Node *node) {
  Out << "Mm";
  mangleSingleChildNode(node); // type
}

void Remangler::mangleClassMetadataBaseOffset(Node *node) {
  Out << "Mo";
  mangleSingleChildNode(node); // type
}

void Remangler::mangleNominalTypeDescriptor(Node *node) {
  Out << "Mn";
  mangleSingleChildNode(node); // type
}

void Remangler::mangleTypeMetadata(Node *node) {
  Out << "M";
  mangleSingleChildNode(node); // type
}

void Remangler::mangleFullTypeMetadata(Node *node) {
  Out << "Mf";
  mangleChildNodes(node); // type
}

void Remangler::mangleProtocolDescriptor(Node *node) {
  Out << "Mp";
  mangleProtocolWithoutPrefix(node->begin()[0]);
}

void Remangler::manglePartialApplyForwarder(Node *node) {
  Out << "PA__T";
  mangleSingleChildNode(node); // global
}

void Remangler::manglePartialApplyObjCForwarder(Node *node) {
  Out << "PAo__T";
  mangleSingleChildNode(node); // global
}

void Remangler::mangleMergedFunction(Node *node) {
  Out << "Tm";
}

void Remangler::mangleDirectness(Node *node) {
  auto getChar = [](Directness d) -> char {
    switch (d) {
    case Directness::Direct: return 'd';
    case Directness::Indirect: return 'i';
    }
    unreachable("bad directness kind");
  };
  Out << getChar(Directness(node->getIndex()));
}

void Remangler::mangleValueWitness(Node *node) {
  const char *Code = nullptr;
  switch (ValueWitnessKind(node->getIndex())) {
#define VALUE_WITNESS(MANGLING, NAME) \
    case ValueWitnessKind::NAME: Code = #MANGLING; break;
#include "swift/Demangling/ValueWitnessMangling.def"
  }
  Out << 'w' << Code;
  mangleSingleChildNode(node); // type
}

void Remangler::mangleValueWitnessTable(Node *node) {
  Out << "WV";
  mangleSingleChildNode(node); // type
}

void Remangler::mangleThrowsAnnotation(Node *node) {
  Out << "z";
}

void Remangler::mangleFieldOffset(Node *node) {
  Out << "Wv";
  mangleChildNodes(node); // directness, entity
}

void Remangler::mangleProtocolWitnessTable(Node *node) {
  Out << "WP";
  mangleSingleChildNode(node); // protocol conformance
}

void Remangler::mangleGenericProtocolWitnessTable(Node *node) {
  Out << "WG";
  mangleSingleChildNode(node); // protocol conformance
}

void Remangler::mangleGenericProtocolWitnessTableInstantiationFunction(
                                                                  Node *node) {
  Out << "WI";
  mangleSingleChildNode(node); // protocol conformance
}

void Remangler::mangleProtocolWitnessTableAccessor(Node *node) {
  Out << "Wa";
  mangleSingleChildNode(node); // protocol conformance
}

void Remangler::mangleLazyProtocolWitnessTableAccessor(Node *node) {
  Out << "Wl";
  mangleChildNodes(node); // type, protocol conformance
}

void Remangler::mangleLazyProtocolWitnessTableCacheVariable(Node *node) {
  Out << "WL";
  mangleChildNodes(node); // type, protocol conformance
}

void Remangler::mangleAssociatedTypeMetadataAccessor(Node *node) {
  Out << "Wt";
  mangleChildNodes(node); // protocol conformance, identifier
}

void Remangler::mangleAssociatedTypeWitnessTableAccessor(Node *node) {
  Out << "WT";
  assert(node->getNumChildren() == 3);
  mangleChildNode(node, 0); // protocol conformance
  mangleChildNode(node, 1); // identifier
  mangleProtocolWithoutPrefix(node->begin()[2]); // type
}

void Remangler::mangleReabstractionThunkHelper(Node *node) {
  Out << "TR";
  if (node->getNumChildren() == 3) Out << 'G';
  mangleChildNodes(node); // generic signature?, type, type
}

void Remangler::mangleReabstractionThunk(Node *node) {
  Out << "Tr";
  if (node->getNumChildren() == 3) Out << 'G';
  mangleChildNodes(node); // generic signature?, type, type
}

void Remangler::mangleProtocolWitness(Node *node) {
  Out << "TW";
  mangleChildNodes(node); // protocol conformance, entity
}

void Remangler::mangleFunction(Node *node, EntityContext &ctx) {
  mangleNamedAndTypedEntity(node, 'F', "", ctx);
}

void Remangler::mangleVariable(Node *node, EntityContext &ctx) {
  mangleNamedAndTypedEntity(node, 'v', "", ctx);
}

void Remangler::mangleSubscript(Node *node, EntityContext &ctx) {
  mangleNamedAndTypedEntity(node, 'i', "", ctx);
}

void Remangler::mangleAccessor(Node *storageNode, StringRef accessorCode,
                               EntityContext &ctx) {
  Out << 'F';
  mangleEntityContext(storageNode->getChild(0), ctx);
  Out << accessorCode;
  switch (storageNode->getKind()) {
  case Demangle::Node::Kind::Variable:
    mangleChildNode(storageNode, 1);
    mangleEntityType(storageNode->getChild(2), ctx);
    break;
  case Demangle::Node::Kind::Subscript:
    if (storageNode->getNumChildren() > 2 &&
        storageNode->getChild(2)->getKind() == Node::Kind::PrivateDeclName) {
      mangleChildNode(storageNode, 2);
    }
    mangleIdentifier("subscript", OperatorKind::NotOperator);
    mangleEntityType(storageNode->getChild(1), ctx);
    break;
  default:
      unreachable("Not a storage node");
  }
}

void Remangler::mangleInitializer(Node *node, EntityContext &ctx) {
  mangleSimpleEntity(node, 'I', "i", ctx);
}

void Remangler::mangleDefaultArgumentInitializer(Node *node,
                                                 EntityContext &ctx) {
  mangleNamedEntity(node, 'I', "A", ctx);
}

void Remangler::mangleDeallocator(Node *node, EntityContext &ctx) {
  mangleSimpleEntity(node, 'F', "D", ctx);
}

void Remangler::mangleDestructor(Node *node, EntityContext &ctx) {
  mangleSimpleEntity(node, 'F', "d", ctx);
}

void Remangler::mangleAllocator(Node *node, EntityContext &ctx) {
  mangleTypedEntity(node, 'F', "C", ctx);
}

void Remangler::mangleConstructor(Node *node, EntityContext &ctx) {
  mangleTypedEntity(node, 'F', "c", ctx);
}

void Remangler::mangleIVarInitializer(Node *node, EntityContext &ctx) {
  mangleSimpleEntity(node, 'F', "e", ctx);
}

void Remangler::mangleIVarDestroyer(Node *node, EntityContext &ctx) {
  mangleSimpleEntity(node, 'F', "E", ctx);
}

void Remangler::mangleGetter(Node *node, EntityContext &ctx) {
  mangleAccessor(node->getFirstChild(), "g", ctx);
}

void Remangler::mangleGlobalGetter(Node *node, EntityContext &ctx) {
  mangleAccessor(node->getFirstChild(), "G", ctx);
}

void Remangler::mangleSetter(Node *node, EntityContext &ctx) {
  mangleAccessor(node->getFirstChild(), "s", ctx);
}

void Remangler::mangleMaterializeForSet(Node *node, EntityContext &ctx) {
  mangleAccessor(node->getFirstChild(), "m", ctx);
}

void Remangler::mangleWillSet(Node *node, EntityContext &ctx) {
  mangleAccessor(node->getFirstChild(), "w", ctx);
}

void Remangler::mangleDidSet(Node *node, EntityContext &ctx) {
  mangleAccessor(node->getFirstChild(), "W", ctx);
}

void Remangler::mangleOwningMutableAddressor(Node *node, EntityContext &ctx) {
  mangleAccessor(node->getFirstChild(), "aO", ctx);
}

void Remangler::mangleNativeOwningMutableAddressor(Node *node,
                                                   EntityContext &ctx) {
  mangleAccessor(node->getFirstChild(), "ao", ctx);
}

void Remangler::mangleNativePinningMutableAddressor(Node *node,
                                                    EntityContext &ctx) {
  mangleAccessor(node->getFirstChild(), "ap", ctx);
}

void Remangler::mangleUnsafeMutableAddressor(Node *node, EntityContext &ctx) {
  mangleAccessor(node->getFirstChild(), "au", ctx);
}

void Remangler::mangleOwningAddressor(Node *node, EntityContext &ctx) {
  mangleAccessor(node->getFirstChild(), "lO", ctx);
}

void Remangler::mangleNativeOwningAddressor(Node *node, EntityContext &ctx) {
  mangleAccessor(node->getFirstChild(), "lo", ctx);
}

void Remangler::mangleNativePinningAddressor(Node *node, EntityContext &ctx) {
  mangleAccessor(node->getFirstChild(), "lp", ctx);
}

void Remangler::mangleUnsafeAddressor(Node *node, EntityContext &ctx) {
  mangleAccessor(node->getFirstChild(), "lu", ctx);
}

void Remangler::mangleExplicitClosure(Node *node, EntityContext &ctx) {
  mangleNamedAndTypedEntity(node, 'F', "U", ctx); // name is index
}

void Remangler::mangleImplicitClosure(Node *node, EntityContext &ctx) {
  mangleNamedAndTypedEntity(node, 'F', "u", ctx); // name is index
}

void Remangler::mangleStatic(Node *node, EntityContext &ctx) {
  Out << 'Z';
  mangleEntityContext(node->getChild(0), ctx);
}

void Remangler::mangleSimpleEntity(Node *node, char basicKind,
                                   StringRef entityKind,
                                   EntityContext &ctx) {
  assert(node->getNumChildren() == 1);
  Out << basicKind;
  mangleEntityContext(node->begin()[0], ctx);
  Out << entityKind;
}

void Remangler::mangleNamedEntity(Node *node, char basicKind,
                                  StringRef entityKind,
                                  EntityContext &ctx) {
  assert(node->getNumChildren() == 2);
  if (basicKind != '\0') Out << basicKind;
  mangleEntityContext(node->begin()[0], ctx);
  Out << entityKind;
  mangleChildNode(node, 1); // decl name / index
}

void Remangler::mangleTypedEntity(Node *node, char basicKind,
                                  StringRef entityKind,
                                  EntityContext &ctx) {
  assert(node->getNumChildren() == 2);
  Out << basicKind;
  mangleEntityContext(node->begin()[0], ctx);
  Out << entityKind;
  mangleEntityType(node->begin()[1], ctx);
}

void Remangler::mangleNamedAndTypedEntity(Node *node, char basicKind,
                                          StringRef entityKind,
                                          EntityContext &ctx) {
  assert(node->getNumChildren() == 3);
  Out << basicKind;
  mangleEntityContext(node->begin()[0], ctx);
  Out << entityKind;
  mangleChildNode(node, 1); // decl name / index
  mangleEntityType(node->begin()[2], ctx);
}

void Remangler::mangleEntityContext(Node *node, EntityContext &ctx) {
  // Remember that we're mangling a context.
  EntityContext::ManglingContextRAII raii(ctx);

  switch (node->getKind()) {
#define NODE(ID)                                \
  case Node::Kind::ID:
#define CONTEXT_NODE(ID)
#include "swift/Demangling/DemangleNodes.def"
    unreachable("not a context node");

#define NODE(ID)
#define CONTEXT_NODE(ID)                        \
  case Node::Kind::ID:                          \
    return mangle##ID(node, ctx);
#include "swift/Demangling/DemangleNodes.def"
  }
  unreachable("bad node kind");
}

void Remangler::mangleEntityType(Node *node, EntityContext &ctx) {
  assert(node->getKind() == Node::Kind::Type);
  assert(node->getNumChildren() == 1);
  node = node->begin()[0];

  // Expand certain kinds of type within the entity context.
  switch (node->getKind()) {
  case Node::Kind::FunctionType:
  case Node::Kind::UncurriedFunctionType: {
    Out << (node->getKind() == Node::Kind::FunctionType ? 'F' : 'f');
    unsigned inputIndex = node->getNumChildren() - 2;
    assert(inputIndex <= 1);
    for (unsigned i = 0; i <= inputIndex; ++i)
      mangle(node->begin()[i]);
    auto returnType = node->begin()[inputIndex+1];
    assert(returnType->getKind() == Node::Kind::ReturnType);
    assert(returnType->getNumChildren() == 1);
    mangleEntityType(returnType->begin()[0], ctx);
    return;
  }
  default:
    mangle(node);
    return;
  }
}

void Remangler::mangleLocalDeclName(Node *node) {
  Out << 'L';
  mangleChildNodes(node); // index, identifier
}

void Remangler::manglePrivateDeclName(Node *node) {
  Out << 'P';
  mangleChildNodes(node); // identifier, identifier
}

void Remangler::mangleTypeMangling(Node *node) {
  Out << 't';
  mangleSingleChildNode(node); // type
}

void Remangler::mangleType(Node *node) {
  mangleSingleChildNode(node);
}

template <size_t N> 
static bool stripPrefix(StringRef &string, const char (&data)[N]) {
  constexpr size_t prefixLength = N - 1;
  if (!string.startswith(StringRef(data, prefixLength)))
    return false;
  string = string.drop_front(prefixLength);
  return true;
}

void Remangler::mangleBuiltinTypeName(Node *node) {
  Out << 'B';
  StringRef text = node->getText();

  if (text == "Builtin.BridgeObject") {
    Out << 'b';
  } else if (text == "Builtin.UnsafeValueBuffer") {
    Out << 'B';
  } else if (text == "Builtin.UnknownObject") {
    Out << 'O';
  } else if (text == "Builtin.NativeObject") {
    Out << 'o';
  } else if (text == "Builtin.RawPointer") {
    Out << 'p';
  } else if (text == "Builtin.Word") {
    Out << 'w';
  } else if (stripPrefix(text, "Builtin.Int")) {
    Out << 'i' << text << '_';
  } else if (stripPrefix(text, "Builtin.Float")) {
    Out << 'f' << text << '_';
  } else if (stripPrefix(text, "Builtin.Vec")) {
    auto split = text.split('x');
    Out << 'v' << split.first << 'B';
    if (split.second == "RawPointer") {
      Out << 'p';
    } else if (stripPrefix(split.second, "Float")) {
      Out << 'f' << split.second << '_';
    } else if (stripPrefix(split.second, "Int")) {
      Out << 'i' << split.second << '_';
    } else {
      unreachable("unexpected builtin vector type");
    }
  } else {
    unreachable("unexpected builtin type");
  }
}

void Remangler::mangleTypeAlias(Node *node, EntityContext &ctx) {
  SubstitutionEntry entry;
  if (trySubstitution(node, entry)) return;
  Out << 'a';
  mangleChildNodes(node); // context, identifier
  addSubstitution(entry);
}

void Remangler::mangleFunctionType(Node *node) {
  Out << 'F';
  mangleChildNodes(node); // argument tuple, result type
}

void Remangler::mangleUncurriedFunctionType(Node *node) {
  Out << 'f';
  mangleChildNodes(node); // argument tuple, result type
}

void Remangler::mangleObjCBlock(Node *node) {
  Out << 'b';
  mangleChildNodes(node); // argument tuple, result type
}

void Remangler::mangleCFunctionPointer(Node *node) {
  Out << 'c';
  mangleChildNodes(node); // argument tuple, result type
}

void Remangler::mangleAutoClosureType(Node *node) {
  Out << 'K';
  mangleChildNodes(node); // argument tuple, result type
}

void Remangler::mangleThinFunctionType(Node *node) {
  Out << "Xf";
  mangleChildNodes(node); // argument tuple, result type
}

void Remangler::mangleArgumentTuple(Node *node) {
  mangleSingleChildNode(node);
}

void Remangler::mangleReturnType(Node *node) {
  mangleSingleChildNode(node);
}

void Remangler::mangleImplFunctionType(Node *node) {
  Out << "XF";
  auto i = node->begin(), e = node->end();
  if (i != e && (*i)->getKind() == Node::Kind::ImplConvention) {
    StringRef text = (*i)->getText();
    i++;
    if (text == "@callee_unowned") {
      Out << 'd';
    } else if (text == "@callee_guaranteed") {
      Out << 'g';
    } else if (text == "@callee_owned") {
      Out << 'o';
    } else {
      unreachable("bad callee convention");
    }
  } else {
    Out << 't';
  }
  for (; i != e &&
         (*i)->getKind() == Node::Kind::ImplFunctionAttribute; ++i) {
    mangle(*i); // impl function attribute
  }
  if (i != e &&
      ((*i)->getKind() == Node::Kind::DependentGenericSignature ||
       (*i)->getKind() == Node::Kind::DependentPseudogenericSignature)) {
    Out << ((*i)->getKind() == Node::Kind::DependentGenericSignature
              ? 'G' : 'g');
    mangleDependentGenericSignature((*i));
    i++;
  }
  Out << '_';
  for (; i != e && (*i)->getKind() == Node::Kind::ImplParameter; ++i) {
    mangleImplParameter(*i);
  }
  Out << '_';
  mangleNodes(i, e); // impl results
  Out << '_';
}

void Remangler::mangleImplFunctionAttribute(Node *node) {
  StringRef text = node->getText();
  if (text == "@convention(block)") {
    Out << "Cb";
  } else if (text == "@convention(c)") {
    Out << "Cc";
  } else if (text == "@convention(method)") {
    Out << "Cm";
  } else if (text == "@convention(objc_method)") {
    Out << "CO";
  } else if (text == "@convention(witness_method)") {
    Out << "Cw";
  } else {
    unreachable("bad impl-function-attribute");
  }
}

void Remangler::mangleImplParameter(Node *node) {
  assert(node->getNumChildren() == 2);
  mangleChildNodes(node); // impl convention, type
}

void Remangler::mangleImplErrorResult(Node *node) {
  assert(node->getNumChildren() == 2);
  Out << 'z';
  mangleChildNodes(node); // impl convention, type
}

void Remangler::mangleImplResult(Node *node) {
  assert(node->getNumChildren() == 2);
  mangleChildNodes(node); // impl convention, type
}

void Remangler::mangleImplEscaping(Node *node) {
  // The old mangler does not encode escaping.
}

void Remangler::mangleImplConvention(Node *node) {
  assert(node->getKind() == Node::Kind::ImplConvention);
  StringRef text = node->getText();
  if (text == "@autoreleased") {
    Out << 'a';
  } else if (text == "@unowned") {
    Out << 'd';
  } else if (text == "@unowned_inner_pointer") {
    Out << 'D'; // only in results
  } else if (text == "@guaranteed") {
    Out << 'g';
  } else if (text == "@deallocating") {
    Out << 'e';
  } else if (text == "@in") {
    Out << 'i'; // only in parameters
  } else if (text == "@out") {
    Out << 'i'; // only in results
  } else if (text == "@inout") {
    Out << 'l';
  } else if (text == "@owned") {
    Out << 'o';
  } else {
    unreachable("invalid impl convention");
  }
}

void Remangler::mangleDynamicSelf(Node *node) {
  Out << 'D';
  mangleSingleChildNode(node); // type
}

void Remangler::mangleErrorType(Node *node) {
  Out << "ERR";
}

void Remangler::mangleSILBoxType(Node *node) {
  Out << 'X' << 'b';
  mangleSingleChildNode(node);
}

void Remangler::mangleMetatype(Node *node) {
  if (node->getNumChildren() == 1) {
    Out << 'M';
    mangleSingleChildNode(node); // type
  } else {
    assert(node->getNumChildren() == 2);
    Out << "XM";
    mangleChildNodes(node); // metatype representation, type
  }
}

void Remangler::mangleExistentialMetatype(Node *node) {
  if (node->getNumChildren() == 1) {
    Out << "PM";
    mangleSingleChildNode(node); // type
  } else {
    assert(node->getNumChildren() == 2);
    Out << "XPM";
    mangleChildNodes(node); // metatype representation, type
  }
}

void Remangler::mangleMetatypeRepresentation(Node *node) {
  StringRef text = node->getText();
  if (text == "@thin") {
    Out << 't';
  } else if (text == "@thick") {
    Out << 'T';
  } else if (text == "@objc_metatype") {
    Out << 'o';
  } else {
    unreachable("bad metatype representation");
  }
}

void Remangler::mangleProtocolList(Node *node) {
  // In its usual use as a type, this gets a prefix 'P'.
  Out << 'P';
  mangleProtocolListWithoutPrefix(node);
}

void Remangler::mangleProtocolListWithoutPrefix(Node *node,
                                                Node *additionalProto) {
  assert(node->getKind() == Node::Kind::ProtocolList);
  assert(node->getNumChildren() == 1);
  auto typeList = node->begin()[0];
  assert(typeList->getKind() == Node::Kind::TypeList);
  for (auto &child : *typeList) {
    mangleProtocolWithoutPrefix(child);
  }
  if (additionalProto) {
    mangleProtocolWithoutPrefix(additionalProto);
  }
  Out << '_';
}

void Remangler::mangleUnowned(Node *node) {
  Out << "Xo";
  mangleSingleChildNode(node); // type
}

void Remangler::mangleUnmanaged(Node *node) {
  Out << "Xu";
  mangleSingleChildNode(node); // type
}

void Remangler::mangleWeak(Node *node) {
  Out << "Xw";
  mangleSingleChildNode(node); // type
}

void Remangler::mangleShared(Node *node) {
  Out << 'h';
  mangleSingleChildNode(node); // type
}

void Remangler::mangleInOut(Node *node) {
  Out << 'R';
  mangleSingleChildNode(node); // type
}

void Remangler::mangleTuple(Node *node) {
  size_t NumElems = node->getNumChildren();
  if (NumElems > 0 &&
      node->getChild(NumElems - 1)->getFirstChild()->getKind() ==
      Node::Kind::VariadicMarker) {
    Out << 't';
  } else {
    Out << 'T';
  }
  mangleChildNodes(node); // tuple elements
  Out << '_';
}

void Remangler::mangleTupleElement(Node *node) {
  mangleChildNodes(node); // tuple element name?, type
}

void Remangler::mangleTupleElementName(Node *node) {
  mangleIdentifier(node->getText(), OperatorKind::NotOperator);
}

void Remangler::mangleDependentGenericType(Node *node) {
  Out << 'u';
  mangleChildNodes(node); // generic signature, type
}

void Remangler::mangleDependentPseudogenericSignature(Node *node) {
  mangleDependentGenericSignature(node);
}

void Remangler::mangleDependentGenericSignature(Node *node) {
  auto i = node->begin(), e = node->end();
  
  // If there's only one generic param, mangle nothing.
  if (node->getNumChildren() >= 1
      && node->getChild(0)->getKind() == Node::Kind::DependentGenericParamCount
      && node->getChild(0)->getIndex() == 1
      && (node->getNumChildren() == 1
          || node->getChild(1)->getKind() != Node::Kind::DependentGenericParamCount))
  {
    ++i;
    goto mangle_requirements;
  }
  
  // Remangle generic params.
  for (; i != e &&
         (*i)->getKind() == Node::Kind::DependentGenericParamCount; ++i) {
    auto count = *i;
    if (count->getIndex() > 0)
      mangleIndex(count->getIndex() - 1);
    else
      Out << 'z';
  }
  
mangle_requirements:
  if (i == e) { // no generic requirements
    Out << 'r';
    return;
  }
  
  Out << 'R';
  mangleNodes(i, e); // generic requirements
  Out << 'r';
}

void Remangler::mangleDependentGenericParamCount(Node *node) {
  unreachable("handled inline in DependentGenericSignature");
}

void Remangler::mangleDependentGenericConformanceRequirement(Node *node) {
  mangleConstrainedType(node->getChild(0));
  // If the constraint represents a protocol, use the shorter mangling.
  if (node->getNumChildren() == 2
      && node->getChild(1)->getKind() == Node::Kind::Type
      && node->getChild(1)->getNumChildren() == 1
      && node->getChild(1)->getChild(0)->getKind() == Node::Kind::Protocol) {
    mangleProtocolWithoutPrefix(node->getChild(1)->getChild(0));
    return;
  }

  mangle(node->getChild(1));
}

void Remangler::mangleDependentGenericSameTypeRequirement(Node *node) {
  mangleConstrainedType(node->getChild(0));
  Out << 'z';
  mangle(node->getChild(1));
}

void Remangler::mangleDependentGenericLayoutRequirement(Node *node) {
  mangleConstrainedType(node->getChild(0));
  Out << 'l';
  auto id =  node->getChild(1)->getText();
  auto size = -1;
  if (node->getNumChildren() > 2) {
    size = node->getChild(2)->getIndex();
  }
  int alignment = -1;
  if (node->getNumChildren() > 3) {
    alignment = node->getChild(3)->getIndex();
  }
  Out << id;
  if (size >= 0)
    Out << size;
  if (alignment >= 0) {
    Out << "_" << alignment;
  }
}

void Remangler::mangleConstrainedType(Node *node) {
  if (node->getFirstChild()->getKind()
        == Node::Kind::DependentGenericParamType) {
    // Can be mangled without an introducer.
    mangleDependentGenericParamIndex(node->getFirstChild());
  } else {
    mangle(node);
  }
}

void Remangler::mangleAssociatedType(Node *node) {
  if (node->hasChildren()) {
    assert(node->getNumChildren() == 1);
    mangleProtocolListWithoutPrefix(*node->begin());
  } else {
    Out << '_';
  }
}

void Remangler::mangleQualifiedArchetype(Node *node) {
  Out << "Qq";
  mangleChildNodes(node); // index, declcontext
}

void Remangler::mangleDeclContext(Node *node) {
  mangleSingleChildNode(node);
}

void Remangler::mangleExtension(Node *node, EntityContext &ctx) {
  assert(node->getNumChildren() == 2 || node->getNumChildren() == 3);
  if (node->getNumChildren() == 3) {
    Out << 'e';
  } else {
    Out << 'E';
  }
  mangleEntityContext(node->begin()[0], ctx); // module
  if (node->getNumChildren() == 3) {
    mangleDependentGenericSignature(node->begin()[2]); // generic sig
  }
  mangleEntityContext(node->begin()[1], ctx); // context
}

void Remangler::mangleModule(Node *node, EntityContext &ctx) {
  SubstitutionEntry entry;
  if (trySubstitution(node, entry)) return;

  // Module types get an M prefix, but module contexts don't.
  if (!ctx.isAsContext()) Out << 'M';
  mangleIdentifier(node->getText(), OperatorKind::NotOperator);
  addSubstitution(entry);
}

void Remangler::mangleAssociatedTypeRef(Node *node) {
  SubstitutionEntry entry;
  if (trySubstitution(node, entry)) return;
  Out << "Q";
  mangleChildNodes(node); // type, identifier
  addSubstitution(entry);
}

void Remangler::mangleDependentMemberType(Node *node) {
  std::vector<Node *> members;
  Node *base = node;
  do {
    members.push_back(base);
    base = base->getFirstChild()->getFirstChild();
  } while (base->getKind() == Node::Kind::DependentMemberType);

  assert(base->getKind() == Node::Kind::DependentGenericParamType
         && "dependent members not based on a generic param are non-canonical"
            " and shouldn't need remangling");
  assert(members.size() >= 1);
  if (members.size() == 1) {
    Out << 'w';
    mangleDependentGenericParamIndex(base);
    mangle(members[0]->getChild(1));
  } else {
    Out << 'W';
    mangleDependentGenericParamIndex(base);

    for (unsigned i = 1, n = members.size(); i <= n; ++i) {
      Node *member = members[n - i];
      mangle(member->getChild(1));
    }
    Out << '_';
  }
}

void Remangler::mangleDependentAssociatedTypeRef(Node *node) {
  SubstitutionEntry entry;
  if (trySubstitution(node, entry)) return;

  if (node->getNumChildren() > 0) {
    Out << 'P';
    mangleProtocolWithoutPrefix(node->getFirstChild());
  }
  mangleIdentifier(node);

  addSubstitution(entry);
}

void Remangler::mangleDependentGenericParamIndex(Node *node) {
  auto depth = node->getChild(0)->getIndex();
  auto index = node->getChild(1)->getIndex();

  if (depth != 0) {
    Out << 'd';
    mangleIndex(depth - 1);
    mangleIndex(index);
    return;
  }
  if (index != 0) {
    mangleIndex(index - 1);
    return;
  }

  // depth == index == 0
  Out << 'x';
}

void Remangler::mangleDependentGenericParamType(Node *node) {
  if (node->getChild(0)->getIndex() == 0
      && node->getChild(1)->getIndex() == 0) {
    Out << 'x';
    return;
  }

  Out << 'q';
  mangleDependentGenericParamIndex(node);
}

void Remangler::mangleIndex(Node *node) {
  mangleIndex(node->getIndex());
}

void Remangler::mangleProtocol(Node *node, EntityContext &ctx) {
  mangleNominalType(node, 'P', ctx);
}

void Remangler::mangleProtocolWithoutPrefix(Node *node) {
  if (node->getKind() == Node::Kind::Type) {
    assert(node->getNumChildren() == 1);
    node = node->begin()[0];
  }

  assert(node->getKind() == Node::Kind::Protocol);
  EntityContext ctx;
  mangleNominalType(node, '\0', ctx);
}

void Remangler::mangleGenericArgs(Node *node, EntityContext &ctx) {
  switch (node->getKind()) {
  case Node::Kind::Structure:
  case Node::Kind::Enum:
  case Node::Kind::Class: {
    NodePointer parentOrModule = node->getChild(0);
    mangleGenericArgs(parentOrModule, ctx);

    // No generic arguments at this level
    Out << '_';
    break;
  }

  case Node::Kind::BoundGenericStructure:
  case Node::Kind::BoundGenericEnum:
  case Node::Kind::BoundGenericClass: {
    NodePointer unboundType = node->getChild(0);
    assert(unboundType->getKind() == Node::Kind::Type);
    NodePointer nominalType = unboundType->getChild(0);
    NodePointer parentOrModule = nominalType->getChild(0);
    mangleGenericArgs(parentOrModule, ctx);

    mangleTypeList(node->getChild(1));
    break;
  }

  default:
    break;
  }
}

void Remangler::mangleAnyNominalType(Node *node, EntityContext &ctx) {
  if (isSpecialized(node)) {
    Out << 'G';

    NodePointer unboundType = getUnspecialized(node, Factory);

    mangleAnyNominalType(unboundType, ctx);
    mangleGenericArgs(node, ctx);
    return;
  }

  switch (node->getKind()) {
  case Node::Kind::Structure:
    mangleNominalType(node, 'V', ctx);
    break;
  case Node::Kind::Enum:
    mangleNominalType(node, 'O', ctx);
    break;
  case Node::Kind::Class:
    mangleNominalType(node, 'C', ctx);
    break;
  default:
    unreachable("bad nominal type kind");
  }
}

void Remangler::mangleStructure(Node *node, EntityContext &ctx) {
  mangleAnyNominalType(node, ctx);
}

void Remangler::mangleEnum(Node *node, EntityContext &ctx) {
  mangleAnyNominalType(node, ctx);
}

void Remangler::mangleClass(Node *node, EntityContext &ctx) {
  mangleAnyNominalType(node, ctx);
}

void Remangler::mangleNominalType(Node *node, char kind, EntityContext &ctx) {
  SubstitutionEntry entry;
  if (trySubstitution(node, entry)) return;
  mangleNamedEntity(node, kind, "", ctx);
  addSubstitution(entry);
}

void Remangler::mangleBoundGenericClass(Node *node) {
  EntityContext ctx;
  mangleAnyNominalType(node, ctx);
}

void Remangler::mangleBoundGenericStructure(Node *node) {
  EntityContext ctx;
  mangleAnyNominalType(node, ctx);
}

void Remangler::mangleBoundGenericEnum(Node *node) {
  EntityContext ctx;
  mangleAnyNominalType(node, ctx);
}

void Remangler::mangleTypeList(Node *node) {
  mangleChildNodes(node); // all types
  Out << '_';
}

void Remangler::mangleReflectionMetadataBuiltinDescriptor(Node *node) {
  Out << "MRb";
}

void Remangler::mangleReflectionMetadataFieldDescriptor(Node *node) {
  Out << "MRf";
}

void Remangler::mangleReflectionMetadataAssocTypeDescriptor(Node *node) {
  Out << "MRa";
}

void Remangler::mangleReflectionMetadataSuperclassDescriptor(Node *node) {
  Out << "MRc";
}

void Remangler::mangleGenericTypeParamDecl(Node *node) {
  unreachable("todo");
}

void Remangler::mangleCurryThunk(Node *node, EntityContext &ctx) {
  Out << "<curry-thunk>";
}

void Remangler::mangleDispatchThunk(Node *node, EntityContext &ctx) {
  Out << "<dispatch-thunk>";
}

void Remangler::mangleEmptyList(Node *node) {
  Out << "<empty>";
}

void Remangler::mangleFirstElementMarker(Node *node) {
  Out << "<first>";
}

void Remangler::mangleVariadicMarker(Node *node) {
  // Handled in mangleTuple
}

void Remangler::mangleOutlinedCopy(Node *node) {
  Out << "Wy";
  mangleChildNodes(node);
}

void Remangler::mangleOutlinedConsume(Node *node) {
  Out << "We";
  mangleChildNodes(node);
}

void Remangler::mangleOutlinedRetain(Node *node) {
  Out << "Wr";
  mangleSingleChildNode(node);
}

void Remangler::mangleOutlinedRelease(Node *node) {
  Out << "Ws";
  mangleSingleChildNode(node);
}

void Remangler::mangleOutlinedInitializeWithTake(Node *node) {
  Out << "Wb";
  mangleSingleChildNode(node);
}

void Remangler::mangleOutlinedInitializeWithCopy(Node *node) {
  Out << "Wc";
  mangleSingleChildNode(node);
}

void Remangler::mangleOutlinedAssignWithTake(Node *node) {
  Out << "Wd";
  mangleSingleChildNode(node);
}

void Remangler::mangleOutlinedAssignWithCopy(Node *node) {
  Out << "Wf";
  mangleSingleChildNode(node);
}

void Remangler::mangleOutlinedDestroy(Node *node) {
  Out << "Wh";
  mangleSingleChildNode(node);
}

void Remangler::mangleOutlinedVariable(Node *node) {
  Out << "Tv" << node->getIndex();
  mangleSingleChildNode(node);
}

void Remangler::mangleOutlinedBridgedMethod(Node *node) {
  Out << "Te" << node->getText();
  mangleSingleChildNode(node);
}

void Remangler::mangleKeyPathGetterThunkHelper(Node *node) {
  Out << "TK";
  mangleChildNodes(node);
}

void Remangler::mangleKeyPathSetterThunkHelper(Node *node) {
  Out << "Tk";
  mangleChildNodes(node);
}

void Remangler::mangleKeyPathEqualsThunkHelper(Node *node) {
  Out << "TH";
  mangleChildNodes(node);
}

void Remangler::mangleKeyPathHashThunkHelper(Node *node) {
  Out << "Th";
  mangleChildNodes(node);
}

void Remangler::mangleProtocolListWithClass(Node *node) {
  Out << "Xc";
  mangleChildNode(node, 1);
  mangleProtocolListWithoutPrefix(node->getChild(0));
}

void Remangler::mangleProtocolListWithAnyObject(Node *node) {
  Node *P = Factory.createNode(Node::Kind::Protocol);
  P->addChild(Factory.createNode(Node::Kind::Module, "Swift"), Factory);
  P->addChild(Factory.createNode(Node::Kind::Identifier, "AnyObject"), Factory);
  Out << "P";
  mangleProtocolListWithoutPrefix(node->getChild(0), /*additionalProto*/ P);
}

void Remangler::mangleVTableThunk(Node *node) {
  Out << "TV";
  mangleChildNodes(node);
}

void Remangler::mangleSILBoxTypeWithLayout(Node *node) {
  assert(node->getKind() == Node::Kind::SILBoxTypeWithLayout);
  assert(node->getNumChildren() == 1 || node->getNumChildren() == 3);
  Out << "XB";
  auto layout = node->getChild(0);
  assert(layout->getKind() == Node::Kind::SILBoxLayout);
  NodePointer genericArgs = nullptr;
  if (node->getNumChildren() == 3) {
    NodePointer signature = node->getChild(1);
    assert(signature->getKind() == Node::Kind::DependentGenericSignature);
    genericArgs = node->getChild(2);
    assert(genericArgs->getKind() == Node::Kind::TypeList);
    
    Out << 'G';
    mangleDependentGenericSignature(signature);
  }
  mangleSILBoxLayout(layout);
  if (genericArgs) {
    for (unsigned i = 0; i < genericArgs->getNumChildren(); ++i) {
      auto type = genericArgs->getChild(i);
      assert(genericArgs->getKind() == Node::Kind::Type);
      mangleType(type);
    }
    Out << '_';  
  }
}

void Remangler::mangleSILBoxLayout(Node *node) {
  assert(node->getKind() == Node::Kind::SILBoxLayout);
  for (unsigned i = 0; i < node->getNumChildren(); ++i) {
    assert(node->getKind() == Node::Kind::SILBoxImmutableField
           || node->getKind() == Node::Kind::SILBoxMutableField);
    mangle(node->getChild(i));
    
  }
  Out << '_';
}

void Remangler::mangleSILBoxMutableField(Node *node) {
  Out << 'm';
  assert(node->getNumChildren() == 1
         && node->getChild(0)->getKind() == Node::Kind::Type);
  mangleType(node->getChild(0));
}

void Remangler::mangleSILBoxImmutableField(Node *node) {
  Out << 'i';
  assert(node->getNumChildren() == 1
         && node->getChild(0)->getKind() == Node::Kind::Type);
  mangleType(node->getChild(0));
}

void Remangler::mangleAssocTypePath(Node *node) {
  unreachable("unsupported");
}

/// The top-level interface to the remangler.
std::string Demangle::mangleNodeOld(const NodePointer &node) {
  if (!node) return "";

  DemanglerPrinter printer;
  Remangler(printer).mangle(node);
  return std::move(printer).str();
}
