#include <torch/csrc/jit/passes/python_print.h>
#include <ATen/core/qualified_name.h>
#include <c10/util/Exception.h>
#include <c10/util/StringUtil.h>
#include <torch/csrc/jit/attributes.h>
#include <torch/csrc/jit/ir.h>
#include <torch/csrc/jit/ir_views.h>
#include <torch/csrc/jit/resource_guard.h>
#include <torch/csrc/jit/script/error_report.h>
#include <torch/csrc/jit/script/module.h>

using c10::QualifiedName;

namespace torch {
namespace jit {

static bool isValidIdentifierChar(char c, size_t pos) {
  return islower(c) || isupper(c) || c == '_' || (pos > 0 && isdigit(c));
}

static bool isValidIdentifier(const std::string& name) {
  if (name.size() == 0)
    return false;
  for (size_t i = 0; i < name.size(); ++i) {
    if (!isValidIdentifierChar(name[i], i))
      return false;
  }
  return true;
}

// some names are valid identifiers but off limits because
// they are keywords or namespaces used in the output
const static std::unordered_set<std::string> reserved_names = {
    // identifiers in the environment while parsing
    "_", // avoid the confusing unnamed _
    "aten",
    "attribute",
    "CONSTANTS",
    "fork",
    "getattr",
    "inf",
    "nan",
    "ops",
    "__torch__",
    // the python keywords
    "and",
    "as",
    "assert",
    "async",
    "await",
    "break",
    "class",
    "continue",
    "def",
    "del",
    "elif",
    "else",
    "except",
    "False",
    "finally",
    "for",
    "from",
    "global",
    "if",
    "import",
    "in",
    "is",
    "lambda",
    "None",
    "nonlocal",
    "not",
    "or",
    "pass",
    "raise",
    "return",
    "True",
    "try",
    "while",
    "with",
    "yield",
    "uninitialized",
    "unchecked_cast",
};

struct PythonPrintImpl {
  using SourceRangeStack = std::vector<SourceRange>;
  SourceRangeStack source_range_stack_ = {SourceRange()};

  struct WithSourceRange {
    explicit WithSourceRange(SourceRangeStack* stack, Node* n) : stack(stack) {
      TORCH_INTERNAL_ASSERT(stack);
      if (auto gen_source = n->sourceRange().findSourceRangeThatGenerated()) {
        stack->push_back(std::move(gen_source.value()));
      } else {
        stack->push_back(n->sourceRange());
      }
    }

    ~WithSourceRange() {
      stack->pop_back();
    }

    SourceRangeStack* stack;
  };

  class TaggedStringStream {
   public:
    TaggedStringStream(const SourceRangeStack* srs) : srs_(srs) {}

    TaggedStringStream& operator<<(const std::string& s) {
      // This prevents having redundant entries at the same offset,
      // which can happen for example in printValueList when begin
      // and end are the empty string.
      if (s.size() == 0) {
        return *this;
      }

      if (!ranges_.size() || ranges_.back().range != srs_->back()) {
        ranges_.emplace_back((size_t)oss_.tellp(), srs_->back());
      }
      oss_ << s;
      return *this;
    }

    TaggedStringStream& operator<<(const TaggedStringStream& rhs) {
      for (const auto& range : rhs.ranges_) {
        if (!ranges_.size() || ranges_.back().range != range.range) {
          ranges_.emplace_back((size_t)oss_.tellp() + range.bytes, range.range);
        }
      }
      oss_ << rhs.oss_.str();
      return *this;
    }

    // This overload is here to prevent people from shooting themselves in the
    // foot. I would be highly surprised if someone actually wanted to write out
    // the address of a TaggedStringStream in the pretty print.
    TaggedStringStream& operator<<(
        const std::shared_ptr<TaggedStringStream>& rhs) {
      (*this) << *rhs;
      return *this;
    }

    template <typename T>
    TaggedStringStream& operator<<(const T& t) {
      if (!ranges_.size() || ranges_.back().range != srs_->back()) {
        ranges_.emplace_back((size_t)oss_.tellp(), srs_->back());
      }
      oss_ << t;
      return *this;
    }

    std::string str() const {
      return oss_.str();
    }

    const std::vector<TaggedRange>& ranges() const {
      return ranges_;
    }

   private:
    std::ostringstream oss_;
    std::vector<TaggedRange> ranges_;
    const SourceRangeStack* srs_;
  };

  // Helper to avoid duplicating class types
  void registerDependency(const c10::NamedTypePtr& type) {
    // Need to do actual equality comparison, not a pointer equality. This is
    // because for some types (e.g. FunctionType), we may have multiple
    // TypePtr's that represent the same underlying thing.
    auto it = std::find_if(
        deps_table_.cbegin(),
        deps_table_.cend(),
        [&](const c10::NamedTypePtr& dep) { return *dep == *type; });

    if (it == deps_table_.cend()) {
      deps_table_.push_back(type);
    }
  }

  // scanValue, scanNode, scanBlock:
  // decide if it is safe to omit the output of a temporary variable,
  // and inline the expression into its use
  // we only do this if
  // (1) it is a constant, or
  // (2) the temporary is unnamed, is single output, is used once,
  //     and would appear in the same order when the expression tree is
  //     reparsed.
  // The last case can be checked
  // because when we emit a expresion tree in the parser,
  // we do a left-to-right postorder traversal of the expression tree (emit
  // children, then emit op). The reverse of this is a right-to-left preorder
  // traversal of the tree. By doing a right-to-left preorder traversal of the
  // inputs of a node, while also scanning the list of emitted nodes backward,
  // we can see if they line up with what would happen when parsed the node as
  // an expression. While they line up we collapse them into an inline
  // expression.

  // The inductive step is that the right-most input should be produced by the
  // node immediatly before the current node if it is in tree order.

  bool canInline(Value* v) {
    Node* n = v->node();
    // there must be only 1 values, otherwise we need an assignment to handle
    // the multiple outout values
    if (n->outputs().size() != 1)
      return false;
    // if it is used more than once, then we need a variable
    if (v->uses().size() != 1)
      return false;
    auto use = v->uses().at(0);
    // if it has a name set, then it was written as a variable so preserve that
    // unless it is being fed directly to the end of the block.
    // in which case it is not as useful to give it a name just to return it
    if (v->hasDebugName() && use.user->kind() != prim::Return)
      return false;
    // don't try to inline control blocks
    if (n->blocks().size() != 0)
      return false;
    // if it is a loop-carried input, we need a variable
    // otherwise the condition or trip count may be emitted in the wrong order
    // w.r.t. to it
    if (use.user->kind() == prim::Loop && use.offset >= 2)
      return false;

    // subgraph may use this more than once, so disable inlining
    if (use.user->kind() == prim::fork)
      return false;

    // isinstance appearing in an if expression
    // causes type refinement to occur, but we have
    // already handled the refinement and inserted cast
    // expressions. By not inlining it into the if condition,
    // we prevent it from happening again.
    if (v->node()->kind() == prim::isinstance) {
      return false;
    }

    return true;
  }

  // block_point is the current node in the reverse linear scan of the emitted
  // nodes v is the current value in the tree traversal that may match with
  // block_point's output.
  Node* scanValue(Node* block_point, Value* v) {
    Node* n = v->node();
    AT_ASSERT(n->kind() == prim::Constant || output_inline_.count(n) == 0);

    if (n == block_point &&
        canInline(v)) { // the node must be at the expected point of the typical
                        // tree traversal
      // recursively see if we can inline the inputs to this input
      block_point = scanNode(block_point);
      output_inline_.insert(n);
    } else if (n->kind() == prim::Constant) {
      // constant nodes can always be inlined, we will de-dup them on parsing
      // and put them at the top of the function regardless
      output_inline_.insert(n);
    }
    return block_point;
  }
  Node* previousNonConstant(Node* n) {
    do {
      n = n->prev();
    } while (n->kind() == prim::Constant);
    return n;
  }

  Node* scanNode(Node* n) {
    // don't bother to scan nodes we have already determined to be inline
    if (output_inline_.count(n)) {
      return n;
    }
    for (auto b : n->blocks()) {
      scanBlock(b);
    }
    Node* block_point = previousNonConstant(n);
    for (auto it = n->inputs().rbegin(), end = n->inputs().rend(); it != end;
         ++it) {
      block_point = scanValue(block_point, *it);
    }
    return block_point;
  }

  void scanBlock(Block* b) {
    scanNode(b->return_node());
    for (auto node : b->nodes().reverse()) {
      scanNode(node);
    }
  }

  size_t getOrAddTensorConstant(at::Tensor t) {
    // XXX - N^2 warning. This code does the exact same thing as
    // ConstantPool, which is also N^2 in the size of the constants,
    // because it doesn't hash any information about the tensors.
    // We will probably need to optimize this at some point using hashing.
    for (size_t i = 0; i < tensor_table_.size(); ++i) {
      if (t.options().type_equal(tensor_table_[i].options()) && t.equal(tensor_table_[i])) {
        return i;
      }
    }
    tensor_table_.emplace_back(std::move(t));
    return tensor_table_.size() - 1;
  }

  std::unordered_set<Node*> seen_constants;
  void buildConstantList(Node* n, std::vector<Node*>& constants) {
    for (auto input : n->inputs()) {
      if (input->node()->kind() == prim::Constant &&
          seen_constants.count(input->node()) == 0) {
        constants.push_back(input->node());
        seen_constants.insert(input->node());
      }
    }
    for (auto b : n->blocks()) {
      buildConstantList(b, constants);
    }
  }
  void buildConstantList(Block* b, std::vector<Node*>& constants) {
    for (auto n : b->nodes())
      buildConstantList(n, constants);
    buildConstantList(b->return_node(), constants);
  }

  // get a new name unique across calls to debugName() and
  // anything we have used.
  std::unordered_map<std::string, size_t> next_id;

  std::string genNameImpl(
      const std::string& candidate,
      std::unordered_set<std::string>& used) {
    std::string name = candidate;
    while (used.count(name) || reserved_names.count(name)) {
      name = candidate + c10::to_string(next_id[name]++);
    }
    used.insert(name);
    return name;
  }
  std::string genName(const std::string& candidate) {
    return genNameImpl(candidate, used_names_);
  }

  // unique names might not be valid identifiers,
  // force them to be by rewriting them
  static std::string makeValidIdentifier(const std::string& candidate) {
    std::stringstream ss;
    if (candidate.size() == 0 || isdigit(candidate[0]))
      ss << "_";
    for (char c : candidate) {
      if (isupper(c) || islower(c) || isdigit(c) || c == '_')
        ss << c;
      else
        ss << '_';
    }
    return ss.str();
  }
  // if we have to assign 'v' a name, what should it be?
  // use the debugName if it was set, otherwise generate a name.
  std::string genUniqueNameFor(Value* v) {
    return genName(
        v->hasDebugName() ? makeValidIdentifier(v->debugNameBase()) : "_");
  }

  // map from Value to how it should be printed at each use
  std::unordered_map<Value*, std::shared_ptr<TaggedStringStream>> expr_table_;
  std::unordered_map<Value*, std::string> ident_refs_;

  // NB: we MUST pass around the shared pointers to these streams by value.
  // There is an interaction in splitLongInlines where the string value for
  // both the RHS and the LHS of an expression are live at the same time,
  // however the value for the RHS is overwritten in the table.
  std::shared_ptr<TaggedStringStream> useOf(Value* v) const {
    // Ident refs take precedent over expression refs, since presence in
    // the ident ref table indicates we have already emitted a statement
    // assigning the given value.
    if (ident_refs_.count(v)) {
      auto rv = std::make_shared<TaggedStringStream>(&source_range_stack_);
      (*rv) << ident_refs_.at(v);
      return rv;
    }
    if (expr_table_.count(v)) {
      return expr_table_.at(v);
    }
    TORCH_INTERNAL_ASSERT(
        false,
        "Value was not present in either expressions"
        " table or ident refs table");
  }
  void assignValue(Value* v, const std::string& s) {
    ident_refs_[v] = s;
  }
  void assignValue(Value* v, std::shared_ptr<TaggedStringStream> s) {
    expr_table_[v] = std::move(s);
  }
  void assignValue(Value* v, Value* w) {
    assignValue(v, useOf(w));
  }
  void assignValuesToTheirUniqueNames(at::ArrayRef<Value*> values) {
    for (auto v : values) {
      assignValue(v, genUniqueNameFor(v));
    }
  }

  size_t level = 0;
  // indent to the current indent level
  TaggedStringStream& indent() {
    for (size_t i = 0; i < level; ++i) {
      body_ << "  ";
    }
    return body_;
  }

  ResourceGuard WithIndented() {
    level++;
    return ResourceGuard([this] { level--; });
  }

  template <class T0, class T1, class F>
  void zipWith(at::ArrayRef<T0> list_a, at::ArrayRef<T1> list_b, F action)
      const {
    auto it_a = list_a.begin();
    auto it_b = list_b.begin();

    if (list_a.size() != list_b.size()) {
      AT_ERROR("Python printer expected 2 lists of same size");
    }

    for (; it_a != list_a.end(); ++it_a, ++it_b) {
      action(*it_a, *it_b);
    }
  }

  void printValueList(
      TaggedStringStream& stmt,
      at::ArrayRef<Value*> list,
      const char* begin = "",
      const char* end = "") {
    stmt << begin;
    auto delimiter = "";
    for (auto* value : list) {
      stmt << delimiter;
      stmt << useOf(value);
      delimiter = ", ";
    }
    stmt << end;
  }

  void printValueIndex(TaggedStringStream& stmt, at::ArrayRef<Value*> inputs) {
    const std::string val_name = useOf(inputs[0])->str();
    if (isValidIdentifier(val_name)) {
      stmt << val_name;
    } else {
      stmt << "(" << val_name << ")";
    }
    stmt << "[";
    stmt << useOf(inputs[1]);
    stmt << "]";
  }

  void printDict(
      TaggedStringStream& stmt,
      at::ArrayRef<Value*> key_value_pairs,
      const char* begin = "{",
      const char* end = "}") {
    stmt << begin;
    auto delimiter = "";
    for (size_t i = 0; i < key_value_pairs.size(); i += 2) {
      stmt << delimiter;
      auto key = key_value_pairs[i];
      auto value = key_value_pairs[i + 1];

      stmt << useOf(key) << ": " << useOf(value);

      delimiter = ", ";
    }
    stmt << end;
  }

  void printAssignment(at::ArrayRef<Value*> lhs, at::ArrayRef<Value*> rhs) {
    if (lhs.size() == 0) {
      return;
    }
    indent();
    printValueList(body_, lhs);
    body_ << " = ";
    printValueList(body_, rhs);
    body_ << "\n";
  }

  bool requiresAnnotation(Value* lhs, Value* rhs) {
    return *lhs->type() != *rhs->type();
  }

  void printAnnotatedAssignment(
      at::ArrayRef<Value*> lhs,
      at::ArrayRef<Value*> rhs) {
    for (size_t i = 0; i < lhs.size(); ++i) {
      indent();
      body_ << useOf(lhs[i]);
      if (requiresAnnotation(lhs[i], rhs[i])) {
        body_ << ": " << lhs[i]->type()->python_str();
      }
      body_ << " = " << useOf(rhs[i]) << "\n";
    }
  }

  void printIf(IfView stmt) {
    assignValuesToTheirUniqueNames(stmt.outputs());
    indent() << "if " << useOf(stmt.cond()) << ":\n";
    {
      auto guard = WithIndented();
      // Print node contents
      printBlock(stmt.thenBlock(), stmt.outputs().size() > 0);
      printAssignment(stmt.outputs(), stmt.thenOutputs());
    }
    indent() << "else:\n";
    {
      auto guard = WithIndented();
      printBlock(stmt.elseBlock(), stmt.outputs().size() > 0);
      printAssignment(stmt.outputs(), stmt.elseOutputs());
    }
  }

  void printLoop(LoopView stmt) {
    // Loop carried dependencies are handled by assigning their initial
    // values to the node->outputs() before the loop,
    // and assign node->outputs() to the new values at the end of each trip.

    auto loop_type = stmt.loopType();
    if (loop_type == LoopView::ModifiedLoop) {
      throw script::ErrorReport(stmt.node()->sourceRange())
          << "loop cannot be printed as python "
          << "because it has gone through an optimization "
          << "that combined while and for loops. File a bug";
    }

    bool emit_as_for_loop = loop_type == LoopView::For;

    assignValuesToTheirUniqueNames(stmt.carriedOutputs());
    // Add aliases for loop-carried dependencies
    zipWith(
        stmt.bodyCarriedInputs(), // Start at 1 to ignore trip count
        stmt.carriedOutputs(),
        [&](Value* block_input, Value* node_output) {
          assignValue(block_input, node_output);
        });

    // Print initial assignments of loop node outputs = loop node inputs
    printAnnotatedAssignment(stmt.carriedOutputs(), stmt.carriedInputs());

    assignValuesToTheirUniqueNames(stmt.currentTripCount());
    // Loop header
    if (emit_as_for_loop) {
      indent();
      body_ << "for " << useOf(stmt.currentTripCount()) << " in range("
            << useOf(stmt.maxTripCount()) << "):\n";
    } else {
      // note: trip_count_in_block is unused because this is a while loop,
      // so we reuse the Value* as a stand-in for the loop condition
      printAssignment(stmt.currentTripCount(), stmt.inputCond());
      indent();
      body_ << "while " << useOf(stmt.currentTripCount()) << ":\n";
    }
    // Loop body
    {
      ResourceGuard indent = WithIndented();
      // Update block outputs to block inputs for next loop iteration
      // skip the assignment to the new condition in for loops because
      // the condition is always True
      size_t offset = emit_as_for_loop ? 1 : 0;
      auto body_block = stmt.bodyBlock();
      ArrayRef<Value*> loop_carried_block_inputs =
          body_block->inputs().slice(offset);
      printBlock(body_block, loop_carried_block_inputs.size() > 0);
      printAssignment(
          loop_carried_block_inputs, body_block->outputs().slice(offset));
    }
  }

  bool isLongLine(const std::string& str) {
    return str.size() + level * 2 >= 40;
  }

  bool isLongInline(Node* node) {
    return output_inline_.count(node) &&
        isLongLine(useOf(node->output())->str());
  }

  bool isNonConstantInline(Value* input) {
    return input->node()->kind() != prim::Constant &&
        output_inline_.count(input->node());
  }

  // [reordering of inlines]
  // We inline anything that is semantically legal to inline, but sometimes
  // we find that these lines get too long. In that case we break the lines
  /// and it  is important that we un-inline all the inputs preceeding the long
  /// input:
  //   r = foo(x.add_(b), some_long + expression)
  //  wrong!
  //   _0 = some_long + expression
  //   r = foo(x.add_(b), _0) # wrong! _0 runs before mutating add_
  // legal!
  //   _0 = x.add_(b)
  //   _1 = some_long + expression
  //   r = foo(_0, _1)
  void splitLongInlines(at::ArrayRef<Value*> inputs) {
    size_t long_inline_slice = 0;
    // find the last input that is too long
    for (size_t i = 0; i < inputs.size(); ++i) {
      if (isLongInline(inputs[i]->node())) {
        long_inline_slice = i + 1;
      }
    }
    // un-inline everything through the last long line
    // constants are ignored since long constants are never inlined in the
    // first place
    for (size_t i = 0; i < long_inline_slice; ++i) {
      if (isNonConstantInline(inputs[i])) {
        printOutputDefinition(inputs[i]->node(), *useOf(inputs[i]));
      }
    }
  }

  template <typename T>
  void printOutputDefinition(Node* node, const T& expr) {
    assignValuesToTheirUniqueNames(node->outputs());
    indent();
    // Print outputs
    if (node->outputs().size() > 0) {
      printValueList(body_, node->outputs());
      body_ << " = ";
    }
    body_ << expr << "\n";
  }

  // Recursively check contained types for any class dependencies
  void registerClassDependencies(const TypePtr& type) {
    if (const auto classType = type->cast<ClassType>()) {
      registerDependency(classType);
    } else if (const auto tupleType = type->cast<TupleType>()) {
      if (tupleType->name()) {
        registerDependency(tupleType);
      }
    } else if (const auto interfaceType = type->cast<InterfaceType>()) {
      registerDependency(interfaceType);
    }
    for (const auto& containedType : type->containedTypes()) {
      registerClassDependencies(containedType);
    }
  }
  void scanTypeDependencies(Node* node) {
    // Check for class dependencies. If this node inputs or outputs a class
    // type, we need to add it to our table of dependencies.
    for (const auto input : node->inputs()) {
      registerClassDependencies(input->type());
    }
    for (const auto output : node->outputs()) {
      registerClassDependencies(output->type());
    }
    for (const auto& name : node->attributeNames()) {
      switch (node->kindOf(name)) {
        case AttributeKind::ty:
          registerClassDependencies(node->ty(name));
          break;
        case AttributeKind::tys:
          for (const TypePtr& t : node->tys(name)) {
            registerClassDependencies(t);
          }
          break;
        default:
          // noop
          break;
      }
    }
  }

  void printNode(Node* node, bool print_const) {
    WithSourceRange guard(&source_range_stack_, node);
    scanTypeDependencies(node);
    if (!print_const && node->kind() == prim::Constant)
      return;
    splitLongInlines(node->inputs());
    switch (node->kind()) {
      case prim::Return:
        if (enforce_importable_ && node->inputs().size() != 1) {
          throw script::ErrorReport(node->sourceRange())
              << "Exportable methods must have a single return value. "
              << "Normal use of ScriptMethods should enforce this";
        }
        if (node->inputs().size() > 0) {
          indent();
          body_ << "return ";
          printValueList(body_, node->inputs());
          body_ << "\n";
        }
        break;
      case prim::Loop:
        printLoop(LoopView(node));
        break;
      case prim::If:
        printIf(IfView(node));
        break;
      case prim::TupleUnpack:
      case prim::ListUnpack:
        assignValuesToTheirUniqueNames(node->outputs());
        indent();
        // TupleUnpack(unpacked) turns into an assignment op that forces
        // the unpack to be inserted when parsed back in:
        // a, b, = unpacked
        // a, = unpacked # trailing comma forces an unpack to happen
        if (node->outputs().size() > 0) {
          printValueList(body_, node->outputs(), "", ", = ");
        }
        body_ << useOf(node->input()) << "\n";
        break;
      case prim::SetAttr: {
        const auto obj = node->inputs().at(0);
        const auto newVal = node->inputs().at(1);
        const auto type = obj->type()->expect<ClassType>();
        const auto& attrname = node->s(attr::name);
        indent();
        body_ << useOf(obj) << "." << attrname << " = " << useOf(newVal)
              << "\n";
      } break;
      case prim::fork: {
        // the subgraph gets emitted as another function
        auto name = genName("__forked_function");
        std::shared_ptr<Graph> graph = node->g(attr::Subgraph);
        indent();
        body_ << "def " << name << "():\n";
        for (size_t i = 0; i < node->inputs().size(); ++i) {
          assignValue(graph->inputs().at(i), node->inputs().at(i));
        }
        printBody(graph->block());
        std::stringstream ss;
        ss << "fork(" << name << ")";
        printOutputDefinition(node, ss.str());
      } break;
      case prim::Function: {
        if (enforce_importable_) {
          throw script::ErrorReport(node->sourceRange())
              << "closures are not exportable";
        }
        assignValuesToTheirUniqueNames(node->outputs());
        auto name = useOf(node->output())->str();
        std::shared_ptr<Graph> graph = node->g(attr::Subgraph);
        indent();
        body_ << "def " << name << "(";
        assignValuesToTheirUniqueNames(graph->inputs());
        for (size_t i = 0; i < graph->inputs().size(); ++i) {
          Value* v = graph->inputs().at(i);
          if (i > 0) {
            body_ << ", ";
          }
          body_ << useOf(v) << ": " << v->type()->python_str();
        }
        body_ << "):\n";
        printBody(graph->block());
      } break;
      default:
        auto ss = std::make_shared<TaggedStringStream>(&source_range_stack_);
        printRHS(*ss, node);

        // we prevent long constants from inlining here.
        // it is not safe to do the same thing for non-constants here
        // because of [reordering of inlines]
        if (output_inline_.count(node) == 0 ||
            (node->kind() == prim::Constant && isLongLine(ss->str()))) {
          printOutputDefinition(node, *ss);
        } else {
          // this node is safe to inline, so assign the output value
          // to that expression directly
          assignValue(node->output(), ss);
        }
    }
  }

  void printConstant(TaggedStringStream& stmt, const IValue& v) {
    const auto customFormatter = [&](std::ostream& ss, const IValue& v) {
      if (v.isTensor()) {
        ss << "CONSTANTS.c" << getOrAddTensorConstant(v.toTensor());
        return true;
      }
      if (v.isTuple() && v.type()->expect<TupleType>()->schema()) {
        // print the namedtuple constructor and let rest of tuple printing
        // continue
        ss << v.type()->expect<TupleType>()->python_str();
      }
      return false;
    };

    std::stringstream ss;
    v.repr(ss, customFormatter);
    stmt << ss.str();
  }

  static bool elementTypeCanBeInferredFromMembers(const TypePtr& elem_type) {
    if (elem_type->kind() == OptionalType::Kind) {
      // it is possible that we are constructing an optional list, but all
      // elements are present
      return false;
    }
    if (elem_type->kind() == InterfaceType::Kind) {
      // since classes can be members of multiple interfaces, we cannot
      // construct which interface the list holds from the members alone
      return false;
    }
    return true;
  }

  void printOpName(TaggedStringStream& stmt, Symbol kind) {
    // Special overriding ops set that requires serializing differently to
    // preserve the original code semantics.
    // This will be more properly handled when we have namespace semantics
    // for serializing the ops, and it right now hard coded these ops to
    // ensure consistency and not breaking BC in the future.
    const static std::unordered_map<Symbol, std::string> override_symbols = {
        {aten::backward, "torch.autograd.backward"},
        {aten::grad, "torch.autograd.grad"},
    };
    if (override_symbols.find(kind) != override_symbols.end()) {
      stmt << override_symbols.at(kind);
    } else if (kind.is_aten()) {
      // special case aten -> torch because we want to rename
      // the aten namespace, but this change will take more time
      // doing it here ensures we do not have fix up archives later
      stmt << "torch." << kind.toUnqualString();
    } else {
      stmt << "ops." << kind.ns().toUnqualString() << "."
           << kind.toUnqualString();
    }
  }

  // Prints the RHS value of a Node, e.g. `aten.add(x, y)`
  void printRHS(TaggedStringStream& stmt, Node* node) {
    switch (node->kind()) {
      case prim::PythonOp: {
        auto value = static_cast<const PythonOp*>(node);
        if (enforce_importable_) {
          throw script::ErrorReport(node->sourceRange())
              << "Could not export Python function call '" << value->name()
              << "'. Remove calls to Python functions before export. "
              << "Did you forget add @script or @script_method annotation? "
              << "If this is a nn.ModuleList, add it to __constants__";
        }
        std::stringstream scalars_stream;
        stmt << "^" << value->name();
        value->writeScalars(scalars_stream);
        stmt << scalars_stream.str();
        printValueList(stmt, node->inputs(), "(", ")");
      } break;
      case prim::Uninitialized: {
        stmt << "uninitialized(" << node->output()->type()->python_str() << ")";
      } break;
      case prim::Constant: {
        if (node->outputs().size() == 1 &&
            node->output()->type()->kind() == TypeKind::FunctionType) {
          auto fn = node->output()->type()->expect<FunctionType>();
          registerDependency(fn);
          stmt << fn->name()->qualifiedName();
        } else if (!node->mustBeNone()) {
          IValue v = toIValue(node->output()).value();
          printConstant(stmt, v);
        } else {
          stmt << "None";
        }
      } break;
      case prim::ImplicitTensorToNum: {
        stmt << "annotate(" << node->output()->type()->python_str() << ", "
             << useOf(node->input()) << ")";
      } break;
      case aten::Int: {
        printValueList(stmt, node->inputs(), "int(", ")");
      } break;
      case aten::Float: {
        printValueList(stmt, node->inputs(), "float(", ")");
      } break;
      case aten::Bool: {
        printValueList(stmt, node->inputs(), "bool(", ")");
      } break;
      case aten::str: {
        printValueList(stmt, node->inputs(), "str(", ")");
      } break;
      case aten::__getitem__: {
        printValueIndex(stmt, node->inputs());
      } break;
      case prim::Print: {
        printValueList(stmt, node->inputs(), "print(", ")");
      } break;
      case aten::sorted: {
        printValueList(stmt, node->inputs(), "sorted(", ")");
      } break;
      case prim::TupleConstruct: {
        if (auto qualname =
                node->output()->type()->expect<TupleType>()->name()) {
          stmt << qualname->qualifiedName();
        }
        printValueList(
            stmt, node->inputs(), "(", node->inputs().size() == 1 ? ",)" : ")");
      } break;
      case prim::TupleIndex: {
        stmt << "(" << useOf(node->inputs().at(0)) << ")["
             << useOf(node->inputs().at(1)) << "]";
      } break;
      case prim::TupleSlice: {
        stmt << "(" << useOf(node->input()) << ")[" << node->i(attr::beg) << ":"
             << node->i(attr::end) << "]";
      } break;
      case prim::ListConstruct: {
        ListTypePtr list_type = node->output()->type()->expect<ListType>();
        TypePtr elem_type = list_type->getElementType();
        // Empty lists must be annotated with their type so the compiler knows
        // what type is supposed to be inside them
        if (node->inputs().size() == 0) {
          stmt << "annotate(" << node->output()->type()->python_str()
               << ", [])";
          // If we can't infer the type based on what's inside, explicitly
          // annotate it to disambiguate.
          // This happens for List[Tensor] vs. List[Optional[Tensor]]
        } else if (!elementTypeCanBeInferredFromMembers(elem_type)) {
          stmt << "annotate(" << node->output()->type()->python_str() << ", ";
          printValueList(stmt, node->inputs(), "[", "]");
          stmt << ")";
          // Otherwise just print a list
        } else {
          printValueList(stmt, node->inputs(), "[", "]");
        }
      } break;
      case prim::DictConstruct: {
        auto dict_type = node->output()->type()->expect<DictType>();
        // There are cases where we must annotate the dict with an explicit type
        // to help the compiler out:
        //   - the dict is empty
        //   - the dict has potentially ambiguous element types
        //       (e.g. Tensor vs. Optional[Tensor])
        if (
            node->inputs().size() == 0 ||
            !elementTypeCanBeInferredFromMembers(dict_type->getKeyType()) ||
            !elementTypeCanBeInferredFromMembers(dict_type->getValueType())) {
          stmt << "annotate(" << node->output()->type()->python_str() << ", ";
          printDict(stmt, node->inputs());
          stmt << ")";
          // Otherwise just print a dict
        } else {
          printDict(stmt, node->inputs());
        }
      } break;
      case prim::CreateObject: {
        const auto classType = node->output()->type()->expect<ClassType>();
        stmt << classType->python_str() << ".__new__("
             << classType->python_str() << ")";
      } break;
      case prim::GetAttr: {
        const auto obj = node->inputs().at(0);
        const auto classType = obj->type()->expect<ClassType>();
        const auto& field = node->s(attr::name);
        if (isValidIdentifier(field)) {
          stmt << useOf(obj) << "." << field;
        } else {
          stmt << "getattr(" << useOf(obj) << ", ";
          std::stringstream field_stream;
          c10::printQuotedString(field_stream, field);
          stmt << field_stream.str() << ")";
        }
      } break;
      case prim::CallFunction: {
        stmt << useOf(node->inputs().at(0)) << "(";
        for (size_t i = 1; i < node->inputs().size(); i++) {
          stmt << useOf(node->inputs()[i]) << ", ";
        }
        stmt << ")";
      } break;
      case prim::CallMethod: {
        const auto& self = node->inputs().at(0);
        const auto& methodName = node->s(attr::name);
        stmt << "(" << useOf(self) << ")"
             << "." << methodName << "(";
        for (size_t i = 1; i < node->inputs().size(); i++) {
          stmt << useOf(node->inputs()[i]) << ", ";
        }
        stmt << ")";

        if (auto selfClass = self->type()->cast<ClassType>()) {
          registerDependency(selfClass);
          const auto method = selfClass->getMethod(node->s(attr::name));
          TORCH_INTERNAL_ASSERT(
              method->qualname() ==
              QualifiedName(selfClass->name()->qualifiedName(), methodName));
        } else if (auto selfInterface = self->type()->cast<InterfaceType>()) {
          registerDependency(selfInterface);
        } else {
          TORCH_INTERNAL_ASSERT(
              false, "method call to unhandled type in serialization");
        }

      } break;
      case aten::_unwrap_optional: {
        printOpName(stmt, node->kind());
        stmt << "(";
        // we cannot recover the type of unwrap_optional(None),
        // using normal schema matching, so we route around this by rewriting
        // the call to unwrap_optional(annotated(Optional[T], None))
        if (node->input()->type()->isSubtypeOf(NoneType::get()) ||
            node->input()->mustBeNone()) {
          auto input_type = OptionalType::create(node->output()->type());
          stmt << "annotate(" << input_type->python_str() << ", "
               << useOf(node->input()) << ")";
        } else {
          stmt << useOf(node->input());
        }
        stmt << ")";
      } break;
      // unchecked_unwrap_optional is no longer generated by the compiler,
      // but may end up here if it was first loaded from a old model and
      // re-saved. On re-save we upgrade it to an unchecked_cast, which is an
      // equivalent op
      case prim::unchecked_unwrap_optional:
      case prim::unchecked_cast: {
        stmt << "unchecked_cast(" << node->output()->type()->python_str()
             << ", " << useOf(node->input()) << ")";
      } break;
      case prim::isinstance: {
        stmt << "isinstance(" << useOf(node->input()) << ", ";
        const auto& types = node->tys(attr::types);
        const auto& kinds = node->ss(attr::kinds);
        if (types.size() == 1 && kinds.size() == 0) {
          stmt << types.at(0)->python_str();
        } else if (kinds.size() == 1 && types.size() == 0) {
          stmt << kinds.at(0);
        } else {
          // check multiple things, e.g. (str, list, int)
          stmt << "(";
          bool first = true;
          for (const TypePtr& typ : types) {
            if (!first) {
              stmt << ", ";
            }
            stmt << typ->python_str();
            first = false;
          }
          for (const std::string& kind : kinds) {
            if (!first) {
              stmt << ", ";
            }
            stmt << kind;
            first = false;
          }
          stmt << ")";
        }
        stmt << ")";
      } break;
      default: {
        printOpName(stmt, node->kind());
        const FunctionSchema& schema = node->schema();
        stmt << "(";
        for (size_t i = 0; i < node->inputs().size(); ++i) {
          if (i > 0) {
            stmt << ", ";
          }
          auto v = useOf(node->inputs().at(i));
          // print the kwarg name if it is a kwarg only argument.
          if (i < schema.arguments().size()) {
            auto arg = schema.arguments().at(i);
            if (arg.kwarg_only()) {
              stmt << arg.name() << "=";
            }
          } else {
            // vararg functions like format can have extra arguments
            AT_ASSERT(schema.is_vararg());
          }
          stmt << *v;
        }
        stmt << ")";
      } break;
    }
  }

  TaggedStringStream& printBlock(Block* root, bool block_has_other_statements) {
    // pythons weird 'pass' syntax creates a bunch of places where we have to
    // check if this block would be empty. But not everything in a block is a
    // node. Sometimes if, loop, and return statements will follow this block
    // and block_has_other_statements == true.
    if (!block_has_other_statements &&
        root->nodes().begin() == root->nodes().end()) {
      indent();
      body_ << "pass\n";
    }
    for (auto* node : root->nodes()) {
      printNode(node, /*print_const=*/false);
    }
    return body_;
  }

  template <typename dtype>
  IValue createBroadList(dtype value, const int64_t& N) {
    c10::List<dtype> repeated;
    repeated.reserve(N);
    for (int i = 0; i < N; ++i) {
      repeated.push_back(value);
    }
    return repeated;
  }

  void printDefaultValue(
      const Argument& arg,
      TaggedStringStream& stmt,
      const IValue& value) {
    stmt << "=";
    // handle broadcasting lists
    if (arg.type()->kind() == ListType::Kind &&
        (value.isInt() || value.isDouble() || value.isBool())) {
      TORCH_INTERNAL_ASSERT(arg.N(), "expected broadcastinglist");
      if (value.isInt()) {
        printConstant(stmt, createBroadList<int64_t>(value.toInt(), *arg.N()));
      } else if (value.isBool()) {
        printConstant(stmt, createBroadList<bool>(value.toBool(), *arg.N()));
      } else if (value.isDouble()) {
        printConstant(
            stmt, createBroadList<double>(value.toDouble(), *arg.N()));
      }
    } else {
      printConstant(stmt, value);
    }
  }

  void printBody(Block* body) {
    // we always print constants at the top of the function, in the order
    // in which they are used.
    std::vector<Node*> constants;
    buildConstantList(body, constants);

    // current graph is used to de-dup names within a single graph
    scanBlock(body);
    {
      auto guard = WithIndented();
      // Print initial constant table (most are just inlined into their use,
      // but some like long strings do get emitted)
      for (Node* n : constants) {
        printNode(n, /*print_const=*/true);
      }
      // Print body
      printBlock(body, body->return_node()->inputs().size() > 0);
      printNode(body->return_node(), /*print_const=*/false);
    }
  }

 public:
  void printFunction(
      const Function& func,
      bool print_first_argument_type = true) {
    const FunctionSchema& schema = func.getSchema();
    Graph& graph = *func.graph();
    used_names_.clear(); // each graph can reuse local names

    WithSourceRange guard(&source_range_stack_, graph.param_node());

    indent();
    body_ << "def " << func.name() << "(";
    auto param_it = graph.inputs().begin();
    for (const Argument& arg : schema.arguments()) {
      std::string arg_name = genName(arg.name());
      if (param_it == graph.inputs().begin()) {
        // the first argument may omit its type when it is implied by context
        // the flag print_first_argument_type determines when to do this
        body_ << arg_name;
        if (print_first_argument_type) {
          body_ << ": " << arg.type()->python_str();
        }
      } else {
        body_ << ",\n    " << arg_name << ": " << arg.type()->python_str();
      }
      if (arg.default_value()) {
        printDefaultValue(arg, body_, *arg.default_value());
      }
      assignValue(*param_it++, arg_name);
    }

    body_ << ") -> " << schema.returns().at(0).type()->python_str() << ":\n";
    printBody(graph.block());
  }

  void printMethod(const Function& func) {
    printFunction(func, /*print_first_argument_type=*/false);
  }

  PythonPrintImpl(
      std::vector<at::Tensor>& tensor_table,
      std::vector<c10::NamedTypePtr>& deps_table,
      bool enforce_importable)
      : body_(&source_range_stack_),
        tensor_table_(tensor_table),
        deps_table_(deps_table),
        enforce_importable_(enforce_importable) {}

  void printClass(const ClassTypePtr& classType) {
    bool is_module = classType->is_module();
    body_ << "class " << classType->name()->name();
    if (is_module) {
      body_ << "(Module)";
    }

    body_ << ":\n";
    {
      const auto guard = WithIndented();
      size_t numAttrs = classType->numAttributes();
      // For modules, we need to print special information about the module's
      // attributes and parameters.
      if (is_module) {
        std::vector<std::string> params;
        // Populate the __parameters__ field. This tells the importer which
        // attributes are parameters.
        for (size_t i = 0; i < numAttrs; i++) {
          if (classType->is_parameter(i)) {
            params.push_back(classType->getAttributeName(i));
          }
        }
        indent();
        body_ << "__parameters__ = [";
        for (const auto& param : params) {
          body_ << "\"" << param << "\", ";
        }
        body_ << "]\n";
      }

      for (size_t i = 0; i < numAttrs; i++) {
        const auto& name = classType->getAttributeName(i);
        const auto& type = classType->getAttribute(i);
        registerClassDependencies(type);

        indent();

        // Handling for when the attribute name is not a valid Python
        // identifier. This happens for, e.g. ModuleList.
        if (!isValidIdentifier(name)) {
          if (i == 0) {
            // Initialize the annotations dict if necessary.
            body_ << "__annotations__ = []\n";
            indent();
          }
          // Print out a direct manipulation of the annotations dict, like:
          //   __annotations__["0"] = SomeType
          body_ << "__annotations__["
                << "\"" << name << "\"] = " << type->python_str() << "\n";
        } else {
          // Otherwise: just emit a python 3 attribute annotation, like:
          //   foo : SomeType
          body_ << name << " : " << type->python_str() << "\n";
        }
      }

      size_t numConstants = classType->numConstants();
      for (size_t i = 0; i < numConstants; i++) {
        const auto& name = classType->getConstantName(i);
        IValue v = classType->getConstant(i);

        indent();
        body_ << name << " : "
              << "Final[" << v.type()->python_str() << "] = ";
        auto ss = std::make_shared<TaggedStringStream>(&source_range_stack_);
        printConstant(*ss, v);
        body_ << ss->str() << "\n";
      }

      // TODO fields
      for (auto& method : classType->methods()) {
        printFunction(*method);
      }
    }
  }

  void printNamedType(const c10::NamedTypePtr& type) {
    if (auto functionType = type->cast<FunctionType>()) {
      printFunction(*functionType->function());
    } else if (auto classType = type->cast<ClassType>()) {
      printClass(classType);
    } else if (auto tupleType = type->cast<TupleType>()) {
      TORCH_INTERNAL_ASSERT(tupleType->schema());
      body_ << "class " << tupleType->name()->name();
      body_ << "(NamedTuple):\n";
      {
        const auto guard = WithIndented();
        for (const auto& attr : tupleType->schema()->arguments()) {
          TORCH_INTERNAL_ASSERT(attr.type());
          indent();
          body_ << attr.name() << " : " << attr.type()->python_str() << "\n";
        }
      }
    } else if (auto interfaceType = type->cast<InterfaceType>()) {
      body_ << "class " << interfaceType->name()->name();
      if (interfaceType->is_module()) {
        body_ << "(ModuleInterface):\n";
      } else {
        body_ << "(Interface):\n";
      }
      {
        auto guard = WithIndented();
        for (const FunctionSchema& method : interfaceType->methods()) {
          indent();
          body_ << "def " << method.name() << "(self";
          TORCH_INTERNAL_ASSERT(
              method.arguments().size() > 0 &&
              method.arguments().at(0).name() == "self");
          for (const Argument& arg :
               at::ArrayRef<Argument>(method.arguments()).slice(1)) {
            auto type = arg.type();
            registerClassDependencies(type);
            body_ << ", " << arg.name() << ": " << type->python_str();
          }
          auto return_type = method.returns().at(0).type();
          registerClassDependencies(return_type);
          body_ << ") -> " << return_type->python_str() << ":\n";
          indent();
          body_ << "  pass\n";
        }
      }
    } else {
      TORCH_INTERNAL_ASSERT(false, "Unhandled NamedType");
    }
  }

  ~PythonPrintImpl() {}

  TaggedStringStream body_;
  // When printing this node, is it safe to write it inline (i.e. without
  // assigning a temporary variable
  std::unordered_set<Node*> output_inline_;

  // what valid identifiers are in use for the current function
  std::unordered_set<std::string> used_names_;

  // constants are written to this table, and given then named CONSTANTS.cN
  // where N is the index into this table.
  std::vector<at::Tensor>& tensor_table_;

  // Any NamedTypes (classes, functions, NamedTuples) used are written to this
  // table.
  std::vector<c10::NamedTypePtr>& deps_table_;

  // when we print this, should we error if the resulting output would
  // not be able to be reparsed?
  bool enforce_importable_;
};

PythonPrint::PythonPrint(
    std::vector<at::Tensor>& tensor_table,
    std::vector<c10::NamedTypePtr>& deps_table,
    bool enforce_importable)
    : pImpl(std::make_shared<PythonPrintImpl>(
          tensor_table,
          deps_table,
          enforce_importable)) {}

void PythonPrint::printNamedType(const c10::NamedTypePtr& type) {
  pImpl->printNamedType(type);
}

void PythonPrint::printFunction(const Function& func) {
  pImpl->printFunction(func);
}

void PythonPrint::printMethod(const Function& func) {
  pImpl->printMethod(func);
}

std::string PythonPrint::str() const {
  return pImpl->body_.str();
}

const SourceRangeRecords& PythonPrint::ranges() const {
  return pImpl->body_.ranges();
}

PythonPrint::~PythonPrint() = default;

bool printerHasSpecialCaseFor(Symbol sym) {
  // WARNING: by adding a value to this set, you are asserting
  // that you have also added special handling of this symbol to
  // the printer above. Not adding handling will cause import and export
  // of modules with this new operator to fail. This is only required
  // for operators without schema. Prefer registering your operator with
  // schema to editing this list here. These cases should only be things
  // that require special handling because they do not fit normal schema
  const static std::unordered_set<Symbol> handled = {
      prim::Constant,
      prim::Uninitialized,
      prim::fork,
      prim::ListConstruct,
      prim::DictConstruct,
      prim::ListUnpack,
      prim::Print,
      prim::PythonOp,
      prim::TupleConstruct,
      prim::TupleIndex,
      prim::TupleSlice,
      prim::TupleUnpack,
      prim::CreateObject,
      prim::GetAttr,
      prim::SetAttr,
      prim::CallFunction,
      prim::isinstance,
      prim::unchecked_cast,
  };

  // WARNING: by adding a value to this set, you are asserting that your
  // primitive is only ever added during optimization and does not need
  // to be correctly printed for export (a process that happens before
  // optimization passes run)
  const static std::unordered_set<Symbol> unneeded = {
      c10::onnx::Reshape, // only used in onnx
      c10::onnx::Shape, // only used in onnx
      prim::AutogradZero, // temporarily inserted by autograd
      prim::AutogradAnyNonZero, // temporarily inserted by autograd
      prim::AutogradAdd, // temporarily inserted by autograd
      prim::ConstantChunk, // optimization pass adds it
      prim::DifferentiableGraph, // optimization pass adds it
      prim::BroadcastSizes, // optimization pass (fuser) adds it
      prim::ChunkSizes, // optimization pass (fuser) adds it
      prim::Drop, // used in interpreter only
      prim::FusedConcat, // optimization pass adds it
      prim::FusionGroup, // optimization pass adds it
      prim::Load, // used in interpreter only
      prim::MMTreeReduce, // used as an optimization
      prim::MMBatchSide, // used as an optimization
      prim::Store, // used in interpreter only
      prim::profile, // used in interpreter only

  };

  // These namespaces are required to have Python printers unless
  // otherwise noted in unneeded.
  const static std::unordered_set<Symbol> required_namespaces = {
      c10::namespaces::prim,
      c10::namespaces::aten,
      c10::namespaces::onnx,
  };

  return handled.count(sym) || unneeded.count(sym) ||
      !required_namespaces.count(sym.ns());
}
} // namespace jit
} // namespace torch
