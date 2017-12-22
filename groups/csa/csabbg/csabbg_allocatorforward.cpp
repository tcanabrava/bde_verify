// csabbg_allocatorforward.cpp                                        -*-C++-*-

#include <clang/AST/ASTContext.h>
#include <clang/AST/Decl.h>
#include <clang/AST/DeclBase.h>
#include <clang/AST/DeclCXX.h>
#include <clang/AST/DeclTemplate.h>
#include <clang/AST/Expr.h>
#include <clang/AST/ExprCXX.h>
#include <clang/AST/Stmt.h>
#include <clang/AST/TemplateBase.h>
#include <clang/AST/TemplateName.h>
#include <clang/AST/Type.h>
#include <clang/ASTMatchers/ASTMatchFinder.h>
#include <clang/ASTMatchers/ASTMatchers.h>
#include <clang/ASTMatchers/ASTMatchersInternal.h>
#include <clang/ASTMatchers/ASTMatchersMacros.h>
#include <clang/Basic/SourceLocation.h>
#include <clang/Basic/Specifiers.h>
#include <csabase_analyser.h>
#include <csabase_debug.h>
#include <csabase_diagnostic_builder.h>
#include <csabase_registercheck.h>
#include <csabase_report.h>
#include <csabase_util.h>
#include <llvm/ADT/APSInt.h>
#include <llvm/ADT/Optional.h>
#include <llvm/ADT/VariadicFunction.h>
#include <llvm/Support/Casting.h>
#include <utils/event.hpp>
#include <utils/function.hpp>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace csabase { class PPObserver; }
namespace csabase { class Visitor; }

using namespace clang;
using namespace clang::ast_matchers;
using namespace clang::ast_matchers::internal;
using namespace csabase;

// -----------------------------------------------------------------------------

static std::string const check_name("allocator-forward");

// -----------------------------------------------------------------------------

namespace {

bool is_allocator(QualType    type,
                  ASTContext& c,
                  bool        includeBases = true,
                  bool        excludeConst = false)
    // Return 'true' iff the specified 'type' is a pointer or reference to an
    // allocator.  If the optionally specified 'includeBases' is false, do not
    // consider base classes of 'type'.  If the optionally specified
    // 'excludeConst' is true, do not consider const-qualified pointers.
{
    static const std::string a1 = "BloombergLP::bslma::Allocator";
    static const std::string a2 = "bsl::allocator";

    type = type.getDesugaredType(c);

    if (type->isPointerType()) {
        type = type->getPointeeType().getDesugaredType(c);
        if (excludeConst && type.isConstQualified()) {
            return false;
        }
    } else if (type->isReferenceType()) {
        type = type->getPointeeType().getDesugaredType(c);
        if (type->isPointerType()) {
            type = type->getPointeeType().getDesugaredType(c);
            if (excludeConst && type.isConstQualified()) {
                return false;
            }
        }
    }
    bool is = false;
    if (auto r = type->getAsCXXRecordDecl()) {
        auto all_true = [](const CXXRecordDecl *decl) { return true; };
        auto not_alloc = [](const CXXRecordDecl *decl) {
            std::string t = decl->getQualifiedNameAsString();
            return t != a1 && t != a2;
        };
        auto rd = r->getDefinition();
        is = !not_alloc(r) ||
             (includeBases &&
              rd &&
              rd->forallBases(all_true) &&
              !rd->forallBases(not_alloc));
    }
    return is;
}

bool is_allocator(const Type& type,
                  ASTContext& c,
                  bool        includeBases = true,
                  bool        excludeConst = false)
{
    return is_allocator(QualType(&type, 0), c, includeBases, excludeConst);
}

bool is_allocator_tag(QualType type, ASTContext& c)
    // Return 'true' iff the specified type is 'bsl::allocator_arg_t'.
{
    static const std::string aa = "bsl::allocator_arg_t";

    if (auto r = type.getDesugaredType(c)->getAsCXXRecordDecl()) {
        if (r->hasDefinition()) {
            return aa == r->getDefinition()->getQualifiedNameAsString();
        }
    }
    return false;
}

bool is_allocator_tag(const Type& type, ASTContext& c)
{
    return is_allocator_tag(QualType(&type, 0), c);
}

}

namespace clang {
namespace ast_matchers {

AST_MATCHER_P(TemplateArgument, equalsIntegral, unsigned, N) {
  return Node.getKind() == TemplateArgument::Integral &&
         Node.getAsIntegral() == N;
}

AST_MATCHER_P(FunctionDecl, hasLastParameter,
               internal::Matcher<ParmVarDecl>, InnerMatcher) {
    unsigned np = 0;
    if (const auto *ctor = llvm::dyn_cast<CXXConstructorDecl>(&Node)) {
        if (ctor->isCopyOrMoveConstructor()) {
            np = 1;
        }
    }

    return Node.getNumParams() > np &&
           InnerMatcher.matches(
               *Node.getParamDecl(Node.getNumParams() - 1), Finder, Builder);
}

AST_MATCHER_P(FunctionDecl, hasFirstParameter,
               internal::Matcher<ParmVarDecl>, InnerMatcher) {
    return Node.getNumParams() > 0 &&
           InnerMatcher.matches(*Node.getParamDecl(0), Finder, Builder);
}

AST_MATCHER_P(FunctionDecl, hasSecondParameter,
               internal::Matcher<ParmVarDecl>, InnerMatcher) {
    return Node.getNumParams() > 1 &&
           InnerMatcher.matches(*Node.getParamDecl(1), Finder, Builder);
}

AST_MATCHER(Type, isAllocator) {
    return is_allocator(Node, Finder->getASTContext(), true, true);
}

AST_MATCHER(Type, isAllocatorTag) {
    return is_allocator_tag(Node, Finder->getASTContext());
}

}
}

namespace
{

enum AllocatorLocation { a_None, a_Last, a_Second };

struct data
    // Data stored for this set of checks.
{
    typedef std::vector<const CXXConstructorDecl*> Ctors;
    Ctors ctors_;
        // The set of constructor declarations seen.

    typedef std::set<const NamedDecl*> DeclsWithAllocatorTrait;
    DeclsWithAllocatorTrait decls_with_true_allocator_trait_;
        // The set of declarations having a true allocator trait.

    DeclsWithAllocatorTrait decls_with_false_allocator_trait_;
        // The set of declarations having a false allocator trait.

    DeclsWithAllocatorTrait decls_with_dependent_allocator_trait_;
        // The set of declarations having a dependent allocator trait.

    typedef std::set<const CXXConstructExpr*> Cexprs;
    Cexprs cexprs_;
        // The set of constructor expressions seen.

    typedef std::map<const Expr *, const CXXCtorInitializer*> Cinits;
    Cinits cinits_;
        // The set of constructor initializers seen.

    typedef std::map<const Type*, AllocatorLocation> TypeTakesAllocator;
    TypeTakesAllocator type_takes_allocator_;
        // A map of whether a type has a constructor with an allocator
        // parameter.

    typedef std::map<const CXXConstructorDecl *, AllocatorLocation>
                       CtorTakesAllocator;
    CtorTakesAllocator ctor_takes_allocator_;
        // A map of whether a constructor has an allocator parameter.

    typedef std::set<const ReturnStmt*> Returns;
    Returns returns_;

    typedef std::set<const VarDecl*> Globals;
    Globals globals_;

    bool traitsDeclared_ = false;
};

struct report : Report<data>
    // This class two static analysis checkers, one to detect object
    // constructors with allocator parameters which do not pass an allocator to
    // the constructors of their base classes and members with allocator
    // parameters, and a second which detects constructor expressions with an
    // explicit allocator argument in which that argument does not initialize
    // an allocator parameter.  It also contains a variety of utility methods
    // used in implementing those checks.
{
    INHERIT_REPORT_CTOR(report, Report, data);

    const CXXRecordDecl *get_record_decl(QualType type);
        // Return the record declaration for the specified 'type' and a null
        // pointer if it does not have one.

    bool is_allocator(QualType type,
                      bool     includeBases = true,
                      bool     excludeConst = false);
        // Return 'true' iff the specified 'type' is pointer or reference to
        // 'bslma::Allocator'.  If the optionally specified 'includeBases' is
        // false, do not consider base classes of 'type'.  If the optionally
        // specified 'excludeConst' is true, do not consider const-qualified
        // pointers.

    bool is_allocator_tag(QualType type);
        // Return 'true' iff the specified 'type' is 'bsl::allocator_arg_t'.

    bool last_arg_is_explicit_allocator(const CXXConstructExpr* call);
        // Return 'true' iff the specified 'call' to a constructor has
        // arguments and the last argument is an explicitly passed allocator.

    AllocatorLocation takes_allocator(QualType type);
        // Return 'true' iff the 'specified' type has a constructor which has a
        // final allocator paramater.

    AllocatorLocation takes_allocator(CXXConstructorDecl const* constructor);
        // Return 'true' iff the specified 'constructor' has a final allocator
        // pointer paramater.

    void match_nested_allocator_trait(const BoundNodes& nodes);
        // Callback for classes with nested allocator traits.

    void match_class_using_allocator(const BoundNodes& nodes);
        // Callback for classes having constructors with allocator parameters.

    void match_allocator_trait(data::DeclsWithAllocatorTrait* set,
                               const BoundNodes& nodes);
        // Method to insert discovered classes with allocator traits contained
        // within the specifed 'nodes' into the specified 'set'.

    void match_negative_allocator_trait(const BoundNodes& nodes);
        // Callback for discovered classes with negative allocator traits
        // contained within the specifed 'nodes'.

    void match_positive_allocator_trait(const BoundNodes& nodes);
        // Callback for discovered classes with positive allocator traits
        // contained within the specifed 'nodes'.

    void match_dependent_allocator_trait(const BoundNodes& nodes);
        // Callback for discovered classes with dependent allocator traits
        // contained within the specifed 'nodes'.

    void match_ctor_expr(const BoundNodes& nodes);
        // Callback for constructor expressions.

    void match_ctor_init(const BoundNodes& nodes);
        // Callback for constructor initializers.

    void match_return_stmt(const BoundNodes& nodes);
        // Callback for return statements.

    void match_var_decl(const BoundNodes& nodes);
        // Callback for variable declarations.

    void match_ctor_decl(const BoundNodes& nodes);
        // Callback for constructor declarations.

    bool hasRVCognate(const FunctionDecl *func);
        // Return 'true' iff the specified 'func' (which returns 'void' and
        // returns a value through a pointer first parameter) has a cognate
        // function that returns by value.

    void match_should_return_by_value(const BoundNodes& nodes);
        // Callback for functions which could return by value instead of
        // through a pointer.

    void operator()();
        // Invoke the checking procedures.

    void check_globals_use_allocator(data::Globals::const_iterator begin,
                                     data::Globals::const_iterator end);

    bool has_public_copy_constructor(const CXXRecordDecl *decl);
        // Whether the class has a publicly accessible copy constructor.

    void check_not_forwarded(data::Ctors::const_iterator begin,
                             data::Ctors::const_iterator end);
        // Invoke the forwarding check on the items in the range from the
        // specified 'begin' up to but not including the specified 'end'.

    void check_not_forwarded(const CXXConstructorDecl *decl);
        // If the specified constructor 'decl' takes an allocator parameter,
        // check whether it passes the parameter to its subobjects.

    void check_not_forwarded(CXXConstructorDecl::init_const_iterator begin,
                             CXXConstructorDecl::init_const_iterator end,
                             const ParmVarDecl* palloc);
        // Check if the items in the sequence from the specified 'begin' up to
        // but not including the specified 'end' are passed the specified
        // 'palloc' allocator parameter.
 
    bool write_allocator_trait(const CXXRecordDecl *record, bool trait);
        // Write out an allocator trait (true or false depending on the
        // specified 'trait') for the specified 'record'.  Return true if the
        // trait was writeen, and false if it was not.

    void check_not_forwarded(const CXXCtorInitializer* init,
                             const ParmVarDecl* palloc);
        // Check if the specified 'init' initializer is passed the specified
        // 'palloc' allocator parameter, if the initialized object is capable
        // of being so initialized.

    std::string parm_name(const ParmVarDecl* parm, int position);
        // Construct a descriptive name string for the specified 'parm'
        // parameter, incorporating the specified 'position'.

    template <typename Iter>
    void check_wrong_parm(Iter begin, Iter end);
        // Check if the items in the sequence from the specified 'begin' up to
        // but not including the specified 'end' initialize a non-allocator
        // parameter from an explicit allocator argument.

    void check_wrong_parm(const CXXConstructExpr *expr);
        // Check whether the specified 'expr' constructor expression contains a
        // final explicit allocator pointer argument used to initialize a non-
        // allocator parameter.  The canonical case is
        //..
        //  struct X {
        //      bdef_Function<void(*)()> d_f;
        //      X(const bdef_Function<void(*)()>& f, bslma::Allocator *a = 0);
        //  };
        //  X x(bslma::Default::defaultAllocator());
        //..
        // typically occurring when a bdef_Function member is added to a class
        // which did not have one.

    template <typename Iter>
    void check_uses_allocator(Iter begin, Iter end);
        // Check if the items in the sequence from the specified 'begin' up to
        // but not including the specified 'end' initialize an allocator
        // parameter from an object allocator.

    void check_uses_allocator(const CXXConstructExpr *expr);
        // Check whether the specified 'expr' constructor expression contains a
        // final explicit allocator pointer used to initialize an allocator
        // parameter from an object allocator.  The canonical case is
        //..
        //  struct X {
        //      bslma::Allocator *allocator() const;
        //      X& operator=(const X& o) {
        //          X(o, allocator()).swap(*this);
        //          return *this;
        //      }
        //  };
        //..
        // which can run out of memory when the object allocator does not free
        // memory (as in sequential allocators).

    template <typename Iter>
    void check_alloc_returns(Iter begin, Iter end);
        // Check that the return statements in the specified half-open range
        // '[ begin .. end )' do not return items that take allocators.

    void check_alloc_return(const ReturnStmt* stmt);
        // Check that the specified return 'stmt' does not return an item that 
        // takes allocators.
};

const CXXRecordDecl *report::get_record_decl(QualType type)
{
    const TemplateSpecializationType *tst =
        llvm::dyn_cast<TemplateSpecializationType>(type.getTypePtr());

    if (tst) {
        type = tst->desugar();
    }

    const SubstTemplateTypeParmType *sttpt =
        llvm::dyn_cast<SubstTemplateTypeParmType>(type.getTypePtr());

    if (sttpt) {
        type = sttpt->desugar();
    }

    const CXXRecordDecl *rdecl = type->getAsCXXRecordDecl();

    if (!rdecl) {
        rdecl = type->getPointeeCXXRecordDecl();
    }

    if (rdecl) {
        rdecl = rdecl->getCanonicalDecl();
    }

    return rdecl;
}

bool report::is_allocator(QualType type, bool includeBases, bool excludeConst)
{
    return ::is_allocator(type, *a.context(), includeBases, excludeConst);
}

bool report::is_allocator_tag(QualType type)
{
    return ::is_allocator_tag(type, *a.context());
}

bool report::last_arg_is_explicit_allocator(const CXXConstructExpr* call)
{
    unsigned n = call ? call->getNumArgs() : 0;
    const Expr *last = n ? call->getArg(n - 1) : 0;
    return last && !last->isDefaultArgument() && is_allocator(last->getType());
}

AllocatorLocation report::takes_allocator(QualType type)
{
    while (type->isArrayType()) {
        type = QualType(type->getArrayElementTypeNoTypeQual(), 0);
    }
    return d.type_takes_allocator_
        [type.getTypePtr()->getCanonicalTypeInternal().getTypePtr()];
}

AllocatorLocation report::takes_allocator(
                                         CXXConstructorDecl const *constructor)
{
    data::CtorTakesAllocator::iterator itr =
        d.ctor_takes_allocator_.find(constructor);
    if (itr != d.ctor_takes_allocator_.end()) {
        return itr->second;
    }

    unsigned n = constructor->getNumParams();

    if (n == 0 || (constructor->isCopyOrMoveConstructor() && n == 1)) {
        return d.ctor_takes_allocator_[constructor] = a_None;         // RETURN
    }

    if (n > 1) {
        QualType t1 = constructor->getParamDecl(0)->getType();
        QualType t2 = constructor->getParamDecl(1)->getType();
        if (is_allocator_tag(t1) && is_allocator(t2, true, true)) {
            return d.ctor_takes_allocator_[constructor] = a_Second;
        }
    }

    QualType type = constructor->getParamDecl(n - 1)->getType();
    return d.ctor_takes_allocator_[constructor] =
               is_allocator(type, true, true) ? a_Last : a_None;
}

static internal::DynTypedMatcher nested_allocator_trait_matcher()
    // Return an AST matcher which looks for nested traits.  Expanded from
    // macros, allocator traits look like:
    //..
    //  class MyClass { operator bslalg::TypeTraitUsesBslmaAllocator::
    //                           NestedTraitDeclaration<MyClass>() const ... };
    //..
    // or
    //..
    //  class MyClass { operator BloombergLP::bslmf::NestedTraitDeclaration<
    //                                      MyClass,
    //                                      bslma::UsesBslmaAllocator,
    //                                      true>() const ... };
    //..
    // In the second case above, the final boolean parameter may also be false
    // or missing.  The details of the classes involved are too hairy to tease
    // out in the AST matcher; instead the matcher looks for a superset of
    // methods and the callback looks for further structure.
{
    return decl(forEachDescendant(
        cxxMethodDecl(
            matchesName("::operator NestedTraitDeclaration($|<)"),
            returns(qualType().bind("type")),
            ofClass(recordDecl().bind("class"))
        ).bind("trait")
    ));
}

void report::match_nested_allocator_trait(const BoundNodes& nodes)
{
    CXXRecordDecl const* decl = nodes.getNodeAs<CXXRecordDecl>("class");
    QualType qt = *nodes.getNodeAs<QualType>("type");
    std::string type = qt.getAsString();

    if (!contains_word(type, decl->getNameAsString())) {
        a.report(nodes.getNodeAs<CXXMethodDecl>("trait"),
                         check_name, "BT01",
                         "Trait declaration does not mention its class '%0'")
            << decl->getNameAsString();
    }

    const NamedDecl *nd = llvm::dyn_cast<NamedDecl>(decl->getCanonicalDecl());

    if (type.find("bslalg::struct TypeTraitUsesBslmaAllocator::"
                  "NestedTraitDeclaration<") == 0 ||
        type.find("bslalg_TypeTraitUsesBslmaAllocator::"
                  "NestedTraitDeclaration<") == 0 ||
        type.find("bdealg_TypeTraitUsesBdemaAllocator::"
                  "NestedTraitDeclaration<") == 0) {
        d.decls_with_true_allocator_trait_.insert(nd);
    } else if (type.find("BloombergLP::bslmf::NestedTraitDeclaration<") == 0 &&
               type.find(", bslma::UsesBslmaAllocator") != type.npos) {
        auto ts = qt->getAs<TemplateSpecializationType>();
        if (ts && ts->getNumArgs() == 3) {
            const TemplateArgument& ta = ts->getArg(2);
            if (!ta.isDependent()) {
                bool value, found;
                if (ta.getKind() == TemplateArgument::Integral) {
                    value = ta.getAsIntegral() != 0;
                    found = true;
                } else if (ta.getKind() == TemplateArgument::Expression &&
                           ta.getAsExpr()->EvaluateAsBooleanCondition(
                               value, *a.context())) {
                    found = true;
                } else {
                    found = false;
                }
                if (found) {
                    if (value) {
                        d.decls_with_true_allocator_trait_.insert(nd);
                    } else {
                        d.decls_with_false_allocator_trait_.insert(nd);
                    }
                    return;
                }
            }
        }
        if (type.find(", bslma::UsesBslmaAllocator, true>") != type.npos ||
            type.find(", bslma::UsesBslmaAllocator>") != type.npos) {
            d.decls_with_true_allocator_trait_.insert(nd);
        } else if (type.find(", bslma::UsesBslmaAllocator, false>") !=
                   type.npos) {
            d.decls_with_false_allocator_trait_.insert(nd);
        } else if (type.find(", bslma::UsesBslmaAllocator,") != type.npos) {
            d.decls_with_dependent_allocator_trait_.insert(nd);
        }
    }
}

static internal::DynTypedMatcher class_using_allocator_matcher()
    // Matcher for classes that have constructors with a final parameter that
    // is a pointer or reference to an allocator or a reference to a class that
    // has such a constructor.
{
    return decl(forEachDescendant(recordDecl(anyOf(
            has(cxxConstructorDecl(hasLastParameter(parmVarDecl(anyOf(
                hasType(pointerType(isAllocator())),
                hasType(referenceType(isAllocator()))
            )))).bind("last")),
            has(cxxConstructorDecl(allOf(
                hasFirstParameter(hasType(isAllocatorTag())),
                hasSecondParameter(parmVarDecl(anyOf(
                    hasType(pointerType(isAllocator())),
                    hasType(referenceType(isAllocator()))
                )))
            )).bind("second"))
    )).bind("class")));
}

void report::match_class_using_allocator(const BoundNodes& nodes)
{
    QualType type = nodes.getNodeAs<CXXRecordDecl>("class")
                        ->getTypeForDecl()
                        ->getCanonicalTypeInternal();
    d.type_takes_allocator_[type.getTypePtr()] = AllocatorLocation(
        (nodes.getNodeAs<Decl>("last")   ? a_Last   : a_None) |
        (nodes.getNodeAs<Decl>("second") ? a_Second : a_None));
}

static internal::DynTypedMatcher allocator_trait_matcher(int value)
{
    return decl(forEachDescendant(
        classTemplateSpecializationDecl(
            hasName("::BloombergLP::bslma::UsesBslmaAllocator"),
            templateArgumentCountIs(1),
            isDerivedFrom(classTemplateSpecializationDecl(
                hasName("::bsl::integral_constant"),
                templateArgumentCountIs(2),
                hasTemplateArgument(0, refersToType(asString("_Bool"))),
                hasTemplateArgument(1, equalsIntegral(value)))))
            .bind("class")));
}

void report::match_allocator_trait(data::DeclsWithAllocatorTrait* set,
                                   const BoundNodes& nodes)
{
    const ClassTemplateSpecializationDecl* td =
        nodes.getNodeAs<ClassTemplateSpecializationDecl>("class");
    QualType arg = td->getTemplateArgs()[0].getAsType();
    const NamedDecl *d = arg->getAsCXXRecordDecl();
    if (!d) {
        const TemplateSpecializationType* tst =
            arg->getAs<TemplateSpecializationType>();
        if (tst) {
            d = tst->getTemplateName().getAsTemplateDecl();
            if (d) {
                d = llvm::dyn_cast<TemplateDecl>(d)->getTemplatedDecl();
            }
        }
    }
    if (d) {
        d = llvm::dyn_cast<NamedDecl>(d->getCanonicalDecl());
        set->insert(d);
    }
}

void report::match_negative_allocator_trait(const BoundNodes& nodes)
{
    match_allocator_trait(&d.decls_with_false_allocator_trait_, nodes);
}

void report::match_positive_allocator_trait(const BoundNodes& nodes)
{
    match_allocator_trait(&d.decls_with_true_allocator_trait_, nodes);
}

static internal::DynTypedMatcher dependent_allocator_trait_matcher()
{
    return decl(forEachDescendant(
        classTemplateSpecializationDecl(
            hasName("::BloombergLP::bslma::UsesBslmaAllocator"),
            templateArgumentCountIs(1),
            unless(isDerivedFrom(classTemplateSpecializationDecl(
                hasName("::bsl::integral_constant"),
                templateArgumentCountIs(2),
                hasTemplateArgument(0, refersToType(asString("_Bool"))),
                anyOf(hasTemplateArgument(1, equalsIntegral(0)),
                      hasTemplateArgument(1, equalsIntegral(1)))))))
            .bind("class")));
}

void report::match_dependent_allocator_trait(const BoundNodes& nodes)
{
    match_allocator_trait(&d.decls_with_dependent_allocator_trait_, nodes);
}

static internal::DynTypedMatcher should_return_by_value_matcher()
{
    return decl(forEachDescendant(
        functionDecl(
            returns(asString("void")),
            hasParameter(
                0,
                parmVarDecl(
                    hasType(pointerType(unless(pointee(isConstQualified())),
                                        unless(pointee(asString("void"))),
                                        unless(pointee(functionType())),
                                        unless(pointee(memberPointerType())))
                                .bind("type")))
                    .bind("parm")),
            anyOf(parameterCountIs(1),
                  hasParameter(
                      1,
                      unless(
                          anyOf(hasType(isInteger()),
                                hasType(pointerType(
                                    unless(pointee(asString("void"))),
                                    unless(pointee(functionType())),
                                    unless(pointee(memberPointerType())))))))),
            anyOf(hasDescendant(binaryOperator(
                      hasOperatorName("="),
                      hasLHS(unaryOperator(
                          hasOperatorName("*"),
                          hasUnaryOperand(ignoringImpCasts(declRefExpr(
                              to(decl(equalsBoundNode("parm")))))))))),
                  hasDescendant(cxxOperatorCallExpr(
                      hasOverloadedOperatorName("="),
                      hasArgument(
                          0,
                          ignoringImpCasts(unaryOperator(
                              hasOperatorName("*"),
                              hasUnaryOperand(ignoringImpCasts(declRefExpr(
                                  to(decl(equalsBoundNode("parm")))))))))))))
            .bind("func")));
}

bool report::hasRVCognate(const FunctionDecl *func)
{
    const DeclContext *parent = func->getLookupParent();
    std::string name = func->getNameAsString();

    DeclContext::decl_iterator declsb = parent->decls_begin();
    DeclContext::decl_iterator declse = parent->decls_end();
    while (declsb != declse) {
        const Decl *decl = *declsb++;
        const FunctionDecl* cfunc = llvm::dyn_cast<FunctionDecl>(decl);
        const FunctionTemplateDecl* ctplt =
            llvm::dyn_cast<FunctionTemplateDecl>(decl);
        if (ctplt) {
            cfunc = ctplt->getTemplatedDecl();
        }
        if (cfunc &&
            cfunc->getNameAsString() == name &&
            cfunc->getNumParams() == func->getNumParams() - 1 &&
            cfunc->getCallResultType() ==
                func->getParamDecl(0)->getOriginalType()->getPointeeType()) {
            return true;
        }
    }
    return false;
}

void report::match_should_return_by_value(const BoundNodes& nodes)
{
    auto func = nodes.getNodeAs<FunctionDecl>("func");
    auto p1 = nodes.getNodeAs<PointerType>("type");
    if (func->getCanonicalDecl() == func &&
        func->getTemplatedKind() == FunctionDecl::TK_NonTemplate &&
        !func->getLocation().isMacroID() &&
        !llvm::dyn_cast<CXXConstructorDecl>(func) &&
        !p1->getPointeeType()->isDependentType() &&
        !p1->getPointeeType()->isInstantiationDependentType() &&
        !p1->getPointeeType()->isAnyCharacterType() &&
        !p1->getPointeeType()->isIntegralType(*a.context()) &&
        !p1->getPointeeType()->isFunctionType() &&
        !p1->getPointeeType()->isMemberPointerType() &&
        !func->getParamDecl(0)->hasDefaultArg() &&
        !is_allocator(p1->desugar()) &&
        !takes_allocator(p1->getPointeeType().getCanonicalType()) &&
        (func->getNumParams() == 1 ||
            func->getParamDecl(0)->getOriginalType().getUnqualifiedType() !=
            func->getParamDecl(1)->getOriginalType().getUnqualifiedType()) &&
        !hasRVCognate(func)) {
        a.report(func, check_name, "RV01",
                 "Consider returning '%0' by value")
            << p1->getPointeeType().getCanonicalType().getAsString();
        a.report(func->getParamDecl(0), check_name, "RV01",
                 "instead of through pointer parameter",
                 false, DiagnosticIDs::Note);
    }
}

static internal::DynTypedMatcher ctor_expr_matcher()
{
    return decl(forEachDescendant(cxxConstructExpr(anything()).bind("e")));
}

void report::match_ctor_expr(const BoundNodes& nodes)
{
    auto expr = nodes.getNodeAs<CXXConstructExpr>("e");
    d.cexprs_.insert(expr);
}

static internal::DynTypedMatcher ctor_init_matcher()
{
    return decl(forEachDescendant(cxxCtorInitializer(anything()).bind("i")));
}

void report::match_ctor_init(const BoundNodes& nodes)
{
    auto init = nodes.getNodeAs<CXXCtorInitializer>("i");
    d.cinits_[init->getInit()] = init;
}

static internal::DynTypedMatcher return_stmt_matcher()
{
    return decl(forEachDescendant(
        functionDecl(forEachDescendant(returnStmt(anything()).bind("r")))
            .bind("f")));
}

void report::match_return_stmt(const BoundNodes& nodes)
{
    //if (!nodes.getNodeAs<FunctionDecl>("f")->isTemplateInstantiation())
    {
        auto stmt = nodes.getNodeAs<ReturnStmt>("r");
        d.returns_.insert(stmt);
    }
}

static internal::DynTypedMatcher var_decl_matcher()
{
    return decl(forEachDescendant(varDecl(anything()).bind("v")));
}

void report::match_var_decl(const BoundNodes& nodes)
{
    auto decl = nodes.getNodeAs<VarDecl>("v");
    if (decl->hasGlobalStorage() &&
        decl->hasInit() &&
        llvm::dyn_cast<CXXConstructExpr>(decl->getInit())) {
        d.globals_.insert(decl);
    }
}

static internal::DynTypedMatcher ctor_decl_matcher()
{
    return decl(forEachDescendant(cxxConstructorDecl(anything()).bind("c")));
}

void report::match_ctor_decl(const BoundNodes& nodes)
{
    auto decl = nodes.getNodeAs<CXXConstructorDecl>("c");
    d.ctors_.push_back(decl);
}

void report::operator()()
{
    MatchFinder mf;

    OnMatch<report, &report::match_nested_allocator_trait> m2(this);
    mf.addDynamicMatcher(nested_allocator_trait_matcher(), &m2);

    OnMatch<report, &report::match_negative_allocator_trait> m4(this);
    mf.addDynamicMatcher(allocator_trait_matcher(0), &m4);

    OnMatch<report, &report::match_positive_allocator_trait> m6(this);
    mf.addDynamicMatcher(allocator_trait_matcher(1), &m6);

    OnMatch<report, &report::match_dependent_allocator_trait> m5(this);
    mf.addDynamicMatcher(dependent_allocator_trait_matcher(), &m5);

    OnMatch<report, &report::match_class_using_allocator> m3(this);
    mf.addDynamicMatcher(class_using_allocator_matcher(), &m3);

    OnMatch<report, &report::match_should_return_by_value> m7(this);
    mf.addDynamicMatcher(should_return_by_value_matcher(), &m7);

    OnMatch<report, &report::match_ctor_expr> m1(this);
    mf.addDynamicMatcher(ctor_expr_matcher(), &m1);

    OnMatch<report, &report::match_ctor_init> m11(this);
    mf.addDynamicMatcher(ctor_init_matcher(), &m11);

    OnMatch<report, &report::match_return_stmt> m8(this);
    mf.addDynamicMatcher(return_stmt_matcher(), &m8);

    OnMatch<report, &report::match_var_decl> m9(this);
    mf.addDynamicMatcher(var_decl_matcher(), &m9);

    OnMatch<report, &report::match_ctor_decl> m10(this);
    mf.addDynamicMatcher(ctor_decl_matcher(), &m10);

    mf.match(*a.context()->getTranslationUnitDecl(), *a.context());

    check_not_forwarded(d.ctors_.begin(), d.ctors_.end());
    check_wrong_parm(d.cexprs_.begin(), d.cexprs_.end());
    check_uses_allocator(d.cexprs_.begin(), d.cexprs_.end());
    check_alloc_returns(d.returns_.begin(), d.returns_.end());

    std::set<data::Globals::value_type, csabase::SortByLocation>
        sorted_globals(
             d.globals_.begin(), d.globals_.end(), csabase::SortByLocation(m));
    check_globals_use_allocator(sorted_globals.begin(), sorted_globals.end());
}

void report::check_globals_use_allocator(data::Globals::const_iterator begin,
                                         data::Globals::const_iterator end)
{
    for (data::Globals::const_iterator itr = begin; itr != end; ++itr) {
        const VarDecl          *decl = *itr;
        const CXXConstructExpr *expr =
            llvm::dyn_cast<CXXConstructExpr>(decl->getInit());
        if (!is_allocator(expr->getType()) &&
            takes_allocator(expr->getType()) == a_Last) {
            unsigned n   = expr->getNumArgs();
            bool     bad = n == 0;
            if (n > 0) {
                const Expr *last = expr->getArg(n - 1);
                bool        result;
                if (last->isDefaultArgument() ||
                    (last->EvaluateAsBooleanCondition(result, *a.context()) &&
                     result == false)) {
                    bad = true;
                }
            }
            if (bad) {
                a.report(decl, check_name, "GA01",
                         "Variable with global storage must be "
                         "initialized with non-default allocator");
            }
        }
    }
}

bool report::has_public_copy_constructor(const CXXRecordDecl *decl)
{
    if (!decl->hasDefinition() || !decl->hasUserDeclaredCopyConstructor()) {
        return true;                                                  // RETURN
    }

    for (auto c : decl->ctors()) {
        if (c->isCopyConstructor() && c->getAccess() == AS_public) {
            return true;                                              // RETURN
        }
    }

    return false;
}

void report::check_not_forwarded(data::Ctors::const_iterator begin,
                                 data::Ctors::const_iterator end)
{
    std::set<std::pair<bool, const CXXRecordDecl *> > records;

    for (auto itr = begin; itr != end; ++itr) {
        const CXXConstructorDecl *decl = *itr;
        const CXXRecordDecl* record = decl->getParent()->getCanonicalDecl();
        bool uses_allocator = takes_allocator(
                   record->getTypeForDecl()->getCanonicalTypeInternal());
        bool has_true_alloc_trait =
            d.decls_with_true_allocator_trait_.count(record);
        bool has_false_alloc_trait =
            d.decls_with_false_allocator_trait_.count(record);
        bool has_dependent_alloc_trait =
            !has_true_alloc_trait &&
            !has_false_alloc_trait &&
            d.decls_with_dependent_allocator_trait_.count(record);
        if (const ClassTemplateSpecializationDecl* ts =
                llvm::dyn_cast<ClassTemplateSpecializationDecl>(record)) {
            const CXXRecordDecl* tr = ts->getSpecializedTemplate()
                                          ->getTemplatedDecl()
                                          ->getCanonicalDecl();
            if (uses_allocator &&
                !has_true_alloc_trait &&
                !has_false_alloc_trait &&
                !has_dependent_alloc_trait &&
                (!tr->hasDefinition() ||
                 takes_allocator(
                     tr->getTypeForDecl()->getCanonicalTypeInternal()))) {
                record = tr;
            }
            if (d.decls_with_true_allocator_trait_.count(tr)) {
                has_true_alloc_trait = true;
            }
            if (d.decls_with_false_allocator_trait_.count(tr)) {
                has_false_alloc_trait = true;
            }
            if (d.decls_with_dependent_allocator_trait_.count(tr)) {
                has_dependent_alloc_trait = true;
            }
        }

        if (has_false_alloc_trait) {
            continue;
        }

        std::pair<bool, const CXXRecordDecl *> rp =
            std::make_pair(uses_allocator, record);

        check_not_forwarded(decl);

        if (records.count(rp) == 0) {
            records.insert(rp);

            if (has_public_copy_constructor(record)) {
                if (!uses_allocator && has_true_alloc_trait) {
                    a.report(record, check_name, "AT01",
                             "Class %0 does not use allocators but has a "
                             "positive allocator trait")
                        << record;
                } else if (uses_allocator &&
                           !has_true_alloc_trait &&
                           !has_dependent_alloc_trait) {
                    a.report(record, check_name, "AT02",
                             "Class %0 uses allocators but does not have an "
                             "allocator trait")
                        << record;
                    write_allocator_trait(record, true);
                }
            }
        }

        if (decl == decl->getCanonicalDecl() &&
            !decl->isMoveConstructor() &&
            uses_allocator &&
            !takes_allocator(decl)) {
            // Warn if the class does not have a constructor that matches this
            // one, but with a final allocator parameter or an initial
            // allocator_arg_t, allocator pair.

            bool found =    // Private copy constructor declarations are OK.
                decl->getAccess() == AS_private &&
                decl->isCopyOrMoveConstructor() &&
                decl->isUserProvided() &&
                !decl->hasBody();

            unsigned num_parms = decl->getNumParams();
            for (auto ci = begin; !found && ci != end; ++ci) {
                const CXXConstructorDecl *ctor = *ci;
                auto r = ctor->getParent()->getCanonicalDecl();
                if (auto ts =
                        llvm::dyn_cast<ClassTemplateSpecializationDecl>(r)) {
                    r = ts->getSpecializedTemplate()
                            ->getTemplatedDecl()
                            ->getCanonicalDecl();
                }
                AllocatorLocation aloc = takes_allocator(ctor);
                if (ctor == ctor->getCanonicalDecl() &&
                    ctor != decl &&
                    r == record) {
                    if (aloc == a_Last &&
                        ctor->getNumParams() == num_parms + 1) {
                        found = true;
                        for (unsigned pi = 0; found && pi < num_parms; ++pi) {
                            if (decl->getParamDecl(pi)->getOriginalType() !=
                                ctor->getParamDecl(pi)->getOriginalType()) {
                                found = false;
                            }
                        }
                    }
                    if (aloc == a_Second &&
                        ctor->getNumParams() == num_parms + 2) {
                        found = true;
                        for (unsigned pi = 0; found && pi < num_parms; ++pi) {
                            if (decl->getParamDecl(pi)->getOriginalType() !=
                                ctor->getParamDecl(pi + 2)
                                    ->getOriginalType()) {
                                found = false;
                            }
                        }
                    }
                }
            }

            if (!found) {
                std::string type =
                    decl->isDefaultConstructor() ? "default " :
                    decl->isCopyConstructor()    ? "copy "    :
                    decl->isMoveConstructor()    ? "move "    :
                                                   "";

                if (decl->isUserProvided()) {
                    a.report(decl, check_name, "AC01",
                             "This " + type + "constructor has no version "
                             "that can be called with an allocator as the "
                             "final argument")
                        << decl;
                }
                else {
                    a.report(decl, check_name, "AC02",
                             "Implicit " + type + "constructor cannot be "
                             "called with an allocator as the final argument")
                        << decl;
                }
            }
        }
    }
}

bool report::write_allocator_trait(const CXXRecordDecl *record, bool trait)
    // Write out an allocator trait for the specified record:
    //    #include <bslma_usesbslmaallocator.h>
    //    ...
    //    namespace BloombergLP {
    //    namespace bslma {
    //
    //    template <> struct UsesBslmaAllocator<rec> : bsl::true_type {};
    // or
    //    template <> struct UsesBslmaAllocator<rec> : bsl::false_type {};
    //
    //    }  // close namespace bslma
    //    }  // close enterprise namespace
{
    if (!a.is_component(record))
      return false;
    std::string name;
    const PrintingPolicy pp = a.context()->getPrintingPolicy();
    llvm::raw_string_ostream os(name);
    record->getNameForDiagnostic(os, pp, true);
    name = os.str();
    if (name.substr(0, 13) == "BloombergLP::") {
        name = name.substr(13);
    }
    if (name.substr(0, 7) == "bslma::") {
        name = name.substr(7);
    }
    std::string::size_type anon;
    while ((anon = name.find("(anonymous namespace)::")) != name.npos) {
      name.erase(anon, 23);
    }
    std::string s;
    llvm::raw_string_ostream ot(s);
    ot << "namespace BloombergLP {\n"
          "namespace bslma {\n"
          "\n";
    if (!d.traitsDeclared_ &&
        a.lookup_name("::BloombergLP::bslma::UsesBslmaAllocator"))
      d.traitsDeclared_ = true;
    if (!d.traitsDeclared_) {
      ot << "template <class Type>\n"
            "struct UsesBslmaAllocator;\n";
      d.traitsDeclared_ = true;
    }
    ot << "template <";
    if (auto ctpsd =
            llvm::dyn_cast<ClassTemplatePartialSpecializationDecl>(record)) {
      auto tpl = ctpsd->getTemplateParameters();
      for (unsigned i = 0; i < tpl->size(); ++i) {
        if (i != 0)
          ot << ", ";
        const NamedDecl *p = tpl->getParam(i);
        if (auto ttp = llvm::dyn_cast<TemplateTypeParmDecl>(p)) {
          ot << (ttp->wasDeclaredWithTypename() ? "typename " : "class ");
          if (ttp->isParameterPack())
            ot << "...";
          if (p->getIdentifier())
            ot << p->getName();
        } else if (auto nttp = llvm::dyn_cast<NonTypeTemplateParmDecl>(p)) {
          // The following code emulates nttp->print(ot), except that call just
          // produces blank output.  I don't know why.
          QualType qt = nttp->getType();
          auto *pet = qt->getAs<PackExpansionType>();
          if (pet)
            qt = pet->getPattern();
          std::string pname;
          if (auto *ii = nttp->getIdentifier())
            pname = ii->getName();
          if (nttp->isParameterPack() || pet)
            pname = "..." + pname;
          qt.print(ot, pp, pname, 0);
        } else if (auto ttp = dyn_cast<TemplateTemplateParmDecl>(p)) {
          ttp->print(ot);
        }
      }
    }
    ot << ">\n"
          "struct UsesBslmaAllocator< "
       << name
       << " > : bsl::"
       << (trait ? "true_type" : "false_type")
       << " { };\n"
          "\n"
          "}  // close namespace bslma\n"
          "}  // close enterprise namespace\n";
    llvm::errs() << ot.str();
    return true;
}

void report::check_not_forwarded(const CXXConstructorDecl *decl)
{
    if (!decl->hasBody()) {
        return;                                                       // RETURN
    }

    AllocatorLocation aloc = takes_allocator(decl);

    if (aloc == a_None) {
        return;                                                       // RETURN
    }

    // The allocator parameter is the last one or the second one.
    const ParmVarDecl *palloc =
        decl->getParamDecl(aloc == a_Last ? decl->getNumParams() - 1 : 1);

    // Iterate through the base and member initializers and report those
    // which take an allocator parameter that we do not pass.

    check_not_forwarded(decl->init_begin(), decl->init_end(), palloc);
}

void report::check_not_forwarded(CXXConstructorDecl::init_const_iterator begin,
                                 CXXConstructorDecl::init_const_iterator end,
                                 const ParmVarDecl* palloc)
{
    while (begin != end) {
        check_not_forwarded(*begin++, palloc);
    }
}

void report::check_not_forwarded(const CXXCtorInitializer* init,
                                 const ParmVarDecl* palloc)
{
    auto ctor_expr = llvm::dyn_cast<CXXConstructExpr>(init->getInit());

    if (!ctor_expr) {
        return;                                                       // RETURN
    }

    AllocatorLocation aloc = takes_allocator(ctor_expr->getConstructor());

    if (a_Second == aloc ||
        (a_Last == aloc && last_arg_is_explicit_allocator(ctor_expr))) {
        // The allocator is explicitly passed last, or it's passed second
        // (where it would not be optional).
        return;                                                       // RETURN
    }

    // Type of object being initialized.
    const Type *type = init->isBaseInitializer() ?
                           init->getBaseClass() :
                       init->isAnyMemberInitializer() ?
                           init->getAnyMember()->getType().getTypePtr() :
                           nullptr;

    if (!type || a_Last != takes_allocator(type->getCanonicalTypeInternal())) {
        return;                                                       // RETURN
    }

    auto rd = get_record_decl(type->getCanonicalTypeInternal());
    while (rd) {
        rd = rd->getCanonicalDecl();
        if (d.decls_with_false_allocator_trait_.count(rd)) {
            return;                                                   // RETURN
        }
        rd = rd->getTemplateInstantiationPattern();
    }

    SourceLocation loc;
    SourceRange range;

    if (init->isWritten()) {
        loc = ctor_expr->getExprLoc();
        range = init->getSourceRange();
    } else {
        loc = palloc->getLocation();
        range = palloc->getSourceRange();
    }

    if (init->isBaseInitializer()) {
        a.report(loc, check_name, "MA01",
                 "Allocator not passed to base %0")
            << init->getBaseClass()->getCanonicalTypeInternal().getAsString()
            << range;
    } else {
        a.report(loc, check_name, "MA02",
                 "Allocator not passed to member %0")
            << init->getAnyMember()->getNameAsString() << range;
    }
}

std::string report::parm_name(const ParmVarDecl* parm, int position)
{
    std::ostringstream s;

    s << "parameter " << position;

    std::string name = parm->getNameAsString();

    if (name.length() > 0) {
        s << " ('" << name << "')";
    }

    return s.str();
}

template <typename Iter>
void report::check_wrong_parm(Iter begin, Iter end)
{
    while (begin != end) {
        check_wrong_parm(*begin++);
    }
}

void report::check_wrong_parm(const CXXConstructExpr *expr)
{
    const CXXConstructorDecl *decl = expr->getConstructor();
    unsigned n = expr->getNumArgs();
    const ParmVarDecl *lastp;
    const Expr *lastarg;

    // For the problem to possibly occur, we need each of the following:
    //: 1 The constructor has at least two parameters.
    //:
    //: 2 The constructor and the constructor expression have the same
    //:   number of parameters/arguments.  (I believe this will always be
    //:   true.)
    //:
    //: 3 The final constructor parameter has a default argument.
    //:
    //: 4 The final constructor argument expression is the default
    //:   argument.
    //:
    //: 5 The type of the final constructor parameter is pointer to
    //:   allocator.

    if (   n >= 2
        && decl->getNumParams() == n
        && (lastp = decl->getParamDecl(n - 1))->hasDefaultArg()
        && (lastarg = expr->getArg(n - 1))->isDefaultArgument()
        && is_allocator(lastp->getType())) {

        // The error will be that the second-to-last parameter is
        // initialized by the allocator.

        const ParmVarDecl* allocp = decl->getParamDecl(n - 1);
        const ParmVarDecl* wrongp = decl->getParamDecl(n - 2);

        // Descend into the expression, looking for a conversion from an
        // allocator.  The details of this come from an examination of the
        // type structure when a test case exhibiting the problem is
        // encountered.  We use a loop because elements of the descent can
        // repeat.

        const Expr* arg = expr->getArg(n - 2);
        for (;;) {
            arg = arg->IgnoreImpCasts();

            if (llvm::dyn_cast<CXXTemporaryObjectExpr>(arg)) {
                // Actual temporary object requested in the code.
                break;
            }

            if (const MaterializeTemporaryExpr* mte =
                           llvm::dyn_cast<MaterializeTemporaryExpr>(arg)) {
                arg = mte->GetTemporaryExpr();
                continue;
            }

            if (const CXXBindTemporaryExpr* bte =
                               llvm::dyn_cast<CXXBindTemporaryExpr>(arg)) {
                arg = bte->getSubExpr();
                continue;
            }

            if (const CXXConstructExpr* ce =
                                   llvm::dyn_cast<CXXConstructExpr>(arg)) {
                unsigned i;
                for (i = ce->getNumArgs(); i > 0; --i) {
                    const Expr* carg = ce->getArg(i - 1);
                    if (!carg->isDefaultArgument()) {
                        // Get the rightmost non-defaulted argument
                        // expression.

                        arg = carg->IgnoreImpCasts();
                        break;
                    }
                }

                if (i > 0) {
                    continue;
                }
            }

            // At this point, we should have stripped off all the outer
            // layers of the argument expression which are performing the
            // conversion to the parameter type, and have the inner
            // expression with its actual type.  If that type is
            // pointer-to-allocator, report the problem if it is new.

            if (is_allocator(arg->getType())) {
                a.report(arg->getExprLoc(), check_name, "AM01",
                         "Allocator argument initializes "
                         "non-allocator %0 of type '%1' rather than "
                         "allocator %2")
                    << parm_name(wrongp, n - 1)
                    << wrongp->getType().getAsString()
                    << parm_name(allocp, n)
                    << arg->getSourceRange();
            }

            break;  // Done.
        }
    }
}

template <typename Iter>
void report::check_uses_allocator(Iter begin, Iter end)
{
    while (begin != end) {
        check_uses_allocator(*begin++);
    }
}

void report::check_uses_allocator(const CXXConstructExpr *expr)
{
    unsigned n = expr->getNumArgs();

    if (n == 0) {
        return;
    }

    const CXXConstructorDecl *decl = expr->getConstructor();

    AllocatorLocation aloc = takes_allocator(decl);

    if (decl->getNumParams() != n || a_None == aloc) {
        return;
    }

    if (d.cinits_.find(expr) != d.cinits_.end()) {
        return;
    }

    const Expr *arg = expr->getArg(aloc == a_Last ? n - 1 : 1);

    if (arg->isDefaultArgument()) {
        return;
    }

    // Descend into the expression, looking for a conversion from an allocator.
    // We use a loop because elements of the descent can repeat.
    for (;;) {
        arg = arg->IgnoreImpCasts();

        if (llvm::dyn_cast<CXXTemporaryObjectExpr>(arg)) {
            // Actual temporary object requested in the code.
            break;
        }

        if (const MaterializeTemporaryExpr *mte =
                llvm::dyn_cast<MaterializeTemporaryExpr>(arg)) {
            arg = mte->GetTemporaryExpr();
            continue;
        }

        if (const CXXBindTemporaryExpr *bte =
                llvm::dyn_cast<CXXBindTemporaryExpr>(arg)) {
            arg = bte->getSubExpr();
            continue;
        }

        if (const CXXConstructExpr *ce =
                llvm::dyn_cast<CXXConstructExpr>(arg)) {
            unsigned i;
            for (i = ce->getNumArgs(); i > 0; --i) {
                const Expr* carg = ce->getArg(i - 1);
                if (!carg->isDefaultArgument()) {
                    // Get the rightmost non-defaulted argument expression.
                    arg = carg->IgnoreImpCasts();
                    break;
                }
            }

            if (i > 0) {
                continue;
            }
        }

        // At this point, we should have stripped off all the outer layers of
        // the argument expression which are performing the conversion to the
        // parameter type, and have the inner expression with its actual type.
        // If that type is bslma::Allocator* (specifically, to eliminate cases
        // such as passing &testAllocator), report the use if it is not a
        // parameter, a null pointer, or a call to a non-member function.

        if (is_allocator(arg->getType(), false)) {
            if (auto dre = llvm::dyn_cast<DeclRefExpr>(arg)) {
                if (llvm::dyn_cast<ParmVarDecl>(dre->getDecl())) {
                    // E.g., X x(basicAllocator);
                    break;
                }
            }
            if (arg->IgnoreCasts()->isNullPointerConstant(
                    *a.context(), arg->NPC_ValueDependentIsNotNull)) {
                // E.g., X x((bslma::Allocator *)0);
                break;
            }
            if (llvm::dyn_cast<CallExpr>(arg) &&
                !llvm::dyn_cast<CXXMemberCallExpr>(arg)) {
                // E.g., X x(bslma::Default::allocator());
                break;
            }
            a.report(arg->getExprLoc(), check_name, "AU01",
                     "Verify whether allocator use is appropriate")
                << arg->getSourceRange();
        }

        break;  // Done.
    }
}

template <typename Iter>
void report::check_alloc_returns(Iter begin, Iter end)
{
    for (Iter itr = begin; itr != end; ++itr) {
        check_alloc_return(*itr);
    }
}

void report::check_alloc_return(const ReturnStmt *stmt)
{
    if (   stmt->getRetValue()
        && !stmt->getRetValue()->getType()->isPointerType()
        && d.decls_with_true_allocator_trait_.count(
            get_record_decl(stmt->getRetValue()->getType()))) {
        const FunctionDecl* func = a.get_parent<FunctionDecl>(stmt);
        if (!func || !func->getReturnType()->isReferenceType()) {
            a.report(stmt, check_name, "AR01",
                     "Type using allocator is returned by value");
        }
    }
}

void subscribe(Analyser& analyser, Visitor&, PPObserver&)
    // Create a callback within the specified 'analyser' which will be invoked
    // after a translation unit has been processed.
{
    analyser.onTranslationUnitDone += report(analyser);
}

}  // close anonymous namespace

static RegisterCheck c1(check_name, &subscribe);

// ----------------------------------------------------------------------------
// Copyright (C) 2014 Bloomberg Finance L.P.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
// ----------------------------- END-OF-FILE ----------------------------------
