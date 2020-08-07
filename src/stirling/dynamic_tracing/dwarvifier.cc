#include "src/stirling/dynamic_tracing/dwarvifier.h"

#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <absl/container/flat_hash_map.h>

#include "src/stirling/obj_tools/dwarf_tools.h"

namespace pl {
namespace stirling {
namespace dynamic_tracing {

using dwarf_tools::ArgInfo;
using dwarf_tools::VarInfo;
using dwarf_tools::VarType;

namespace {

// Special variables all end with an underscore to minimize chance of conflict with user variables.
// User variables that end with underscore are not allowed (this is not yet enforced).
constexpr char kSPVarName[] = "sp_";
constexpr char kTGIDVarName[] = "tgid_";
constexpr char kTGIDPIDVarName[] = "tgid_pid_";
constexpr char kTGIDStartTimeVarName[] = "tgid_start_time_";
constexpr char kGOIDVarName[] = "goid_";
constexpr char kKTimeVarName[] = "time_";
constexpr char kStartKTimeNSVarName[] = "start_ktime_ns";
constexpr char kRCVarName[] = "rc_";

// WARNING: Do not change the name of kKTimeVarName above, as it is a name implicitly used by the
//          query engine as the time column.

StatusOr<std::string> BPFHelperVariableName(ir::shared::BPFHelper builtin) {
  static const absl::flat_hash_map<ir::shared::BPFHelper, std::string_view> kBuiltinVarNames = {
      {ir::shared::BPFHelper::GOID, kGOIDVarName},
      {ir::shared::BPFHelper::TGID, kTGIDVarName},
      {ir::shared::BPFHelper::TGID_PID, kTGIDPIDVarName},
      {ir::shared::BPFHelper::TGID_START_TIME, kTGIDStartTimeVarName},
      {ir::shared::BPFHelper::KTIME, kKTimeVarName},
  };
  auto iter = kBuiltinVarNames.find(builtin);
  if (iter == kBuiltinVarNames.end()) {
    return error::NotFound("BPFHelper '$0' does not have a predefined variable",
                           magic_enum::enum_name(builtin));
  }
  return std::string(iter->second);
}

}  // namespace

/**
 * The Dwarvifier generates a Probe from a given LogicalProbe spec.
 * The Dwarvifier's job is to:
 *  - To generate variables (with correct offsets) to access argument/return value expressions.
 *  - To add type information to maps and outputs.
 *  - To create the necessary structs to access those maps and outputs.
 * Any referenced maps and outputs must exist in the Logical spec.
 */
class Dwarvifier {
 public:
  Dwarvifier(const std::map<std::string, ir::shared::Map*>& maps,
             const std::map<std::string, ir::physical::PerfBufferOutput*>& outputs)
      : maps_(maps), outputs_(outputs) {}
  Status Setup(const ir::shared::DeploymentSpec& deployment_spec, ir::shared::Language language);
  Status GenerateProbe(const ir::logical::Probe input_probe, ir::physical::Program* output_program);

 private:
  Status ProcessProbe(const ir::logical::Probe& input_probe, ir::physical::Program* output_program);
  Status ProcessTracepoint(const ir::shared::TracePoint& trace_point,
                           ir::physical::Probe* output_probe);
  Status AddSpecialVariables(ir::physical::Probe* output_probe);
  Status AddStandardVariables(ir::physical::Probe* output_probe);
  Status AddRetProbeVariables(ir::physical::Probe* output_probe);
  Status ProcessConstants(const ir::logical::Constant& constant, ir::physical::Probe* output_probe);

  // The input components describes a sequence of field of nesting structures. The first component
  // is the name of an input argument of a function, or an expression to describe the index of an
  // return value of the function.
  Status ProcessVarExpr(const std::string& var_name, const ArgInfo* arg_info,
                        const std::string& base_var,
                        const std::vector<std::string_view>& components,
                        ir::physical::Probe* output_probe);

  Status ProcessArgExpr(const ir::logical::Argument& arg, ir::physical::Probe* output_probe);
  Status ProcessRetValExpr(const ir::logical::ReturnValue& ret_val,
                           ir::physical::Probe* output_probe);
  Status ProcessMapVal(const ir::logical::MapValue& map_val, ir::physical::Probe* output_probe);
  Status ProcessFunctionLatency(const ir::shared::FunctionLatency& latency,
                                ir::physical::Probe* output_probe);
  Status ProcessStashAction(const ir::logical::MapStashAction& stash_action,
                            ir::physical::Probe* output_probe,
                            ir::physical::Program* output_program);
  Status ProcessDeleteAction(const ir::logical::MapDeleteAction& stash_action,
                             ir::physical::Probe* output_probe);
  Status ProcessOutputAction(const ir::logical::OutputAction& output_action,
                             ir::physical::Probe* output_probe,
                             ir::physical::Program* output_program);

  ir::physical::ScalarVariable* AddVariable(ir::physical::Probe* probe, const std::string& name,
                                            ir::shared::ScalarType type);

  Status GenerateMapValueStruct(const ir::logical::MapStashAction& stash_action_in,
                                const std::string& struct_type_name,
                                ir::physical::Program* output_program);
  Status GenerateOutputStruct(const ir::logical::OutputAction& output_action_in,
                              const std::string& struct_type_name,
                              ir::physical::Program* output_program);

  StatusOr<ir::shared::ScalarType> VarTypeToProtoScalarType(const VarType& type,
                                                            std::string_view name);

  const std::map<std::string, ir::shared::Map*>& maps_;
  const std::map<std::string, ir::physical::PerfBufferOutput*>& outputs_;
  std::map<std::string, ir::physical::Struct*> structs_;

  std::unique_ptr<dwarf_tools::DwarfReader> dwarf_reader_;

  std::map<std::string, dwarf_tools::ArgInfo> args_map_;
  dwarf_tools::RetValInfo retval_info_;

  // All defined ScalarVariable.
  absl::flat_hash_map<std::string, ir::shared::ScalarType> scalar_var_types_;

  // Dwarf and BCC have an 8 byte difference in where they believe the SP is.
  // This adjustment factor accounts for that difference.
  static constexpr int32_t kSPOffset = 8;

  ir::shared::Language language_;
  std::vector<std::string> implicit_columns_;

  // We use these values as we build temporary variables for expressions.

  // String for . operator (eg. my_struct.field)
  static constexpr std::string_view kDotStr = "_D_";

  // String for * operator (eg. (*my_struct).field)
  static constexpr std::string_view kDerefStr = "_X_";
};

// Returns the struct name associated with an Output or Map declaration.
std::string StructTypeName(const std::string& obj_name) {
  return absl::StrCat(obj_name, "_value_t");
}

// AddDwarves is the main entry point.
StatusOr<ir::physical::Program> AddDwarves(const ir::logical::TracepointDeployment& input) {
  if (input.tracepoints_size() != 1) {
    return error::InvalidArgument("Right now only support exactly 1 Tracepoint, got '$0'",
                                  input.tracepoints_size());
  }

  // Index globals for quick lookups.
  std::map<std::string, ir::shared::Map*> maps;
  std::map<std::string, ir::physical::PerfBufferOutput*> outputs;

  ir::physical::Program output_program;

  output_program.mutable_deployment_spec()->CopyFrom(input.deployment_spec());
  output_program.set_language(input.tracepoints(0).program().language());

  // For each input Tracepoint, populates additional variables to chase from predefined registers.
  for (const auto& input_tracepoint : input.tracepoints()) {
    const auto& input_program = input_tracepoint.program();

    // Copy all maps.
    for (const auto& map : input_program.maps()) {
      auto* m = output_program.add_maps();
      m->CopyFrom(map);
      maps[m->name()] = m;
    }

    // Copy all outputs.
    for (const auto& output : input_program.outputs()) {
      auto* o = output_program.add_outputs();

      o->set_name(output.name());
      o->mutable_fields()->CopyFrom(output.fields());
      // Also insert the name of the struct that holds the output variables.
      o->set_struct_type(StructTypeName(output.name()));

      outputs[o->name()] = o;
    }

    // Transform probes.
    Dwarvifier dwarvifier(maps, outputs);
    PL_RETURN_IF_ERROR(dwarvifier.Setup(input.deployment_spec(), input_program.language()));
    for (const auto& probe : input_program.probes()) {
      PL_RETURN_IF_ERROR(dwarvifier.GenerateProbe(probe, &output_program));
    }
  }

  return output_program;
}

// Map to convert Go Base types to ScalarType.
// clang-format off
const absl::flat_hash_map<std::string_view, ir::shared::ScalarType> kGoTypesMap = {
        {"bool",    ir::shared::ScalarType::BOOL},
        {"int",     ir::shared::ScalarType::INT},
        {"int8",    ir::shared::ScalarType::INT8},
        {"int16",   ir::shared::ScalarType::INT16},
        {"int32",   ir::shared::ScalarType::INT32},
        {"int64",   ir::shared::ScalarType::INT64},
        {"uint",    ir::shared::ScalarType::UINT},
        {"uint8",   ir::shared::ScalarType::UINT8},
        {"uint16",  ir::shared::ScalarType::UINT16},
        {"uint32",  ir::shared::ScalarType::UINT32},
        {"uint64",  ir::shared::ScalarType::UINT64},
        {"float32", ir::shared::ScalarType::FLOAT},
        {"float64", ir::shared::ScalarType::DOUBLE},
};

// TODO(oazizi): Keep building this map out.
// Reference: https://en.cppreference.com/w/cpp/language/types
const absl::flat_hash_map<std::string_view, ir::shared::ScalarType> kCPPTypesMap = {
        {"bool",                   ir::shared::ScalarType::BOOL},

        {"short",                  ir::shared::ScalarType::SHORT},
        {"unsigned short",         ir::shared::ScalarType::USHORT},
        {"int",                    ir::shared::ScalarType::INT},
        {"unsigned int",           ir::shared::ScalarType::UINT},
        {"long int",               ir::shared::ScalarType::LONG},
        {"long unsigned int",      ir::shared::ScalarType::ULONG},
        {"long long int",          ir::shared::ScalarType::LONGLONG},
        {"long long unsigned int", ir::shared::ScalarType::ULONGLONG},

        {"char",                   ir::shared::ScalarType::CHAR},
        {"signed char",            ir::shared::ScalarType::CHAR},
        {"unsigned char",          ir::shared::ScalarType::UCHAR},

        {"double",                 ir::shared::ScalarType::DOUBLE},
        {"float",                  ir::shared::ScalarType::FLOAT},
};

const absl::flat_hash_map<std::string_view, ir::shared::ScalarType> kEmptyTypesMap = {};
// clang-format on

const absl::flat_hash_map<std::string_view, ir::shared::ScalarType>& GetTypesMap(
    ir::shared::Language language) {
  switch (language) {
    case ir::shared::Language::GOLANG:
      return kGoTypesMap;
    case ir::shared::Language::C:
    case ir::shared::Language::CPP:
      return kCPPTypesMap;
    default:
      return kEmptyTypesMap;
  }
}

StatusOr<ir::shared::ScalarType> Dwarvifier::VarTypeToProtoScalarType(const VarType& type,
                                                                      std::string_view name) {
  switch (type) {
    case VarType::kBaseType: {
      auto& types_map = GetTypesMap(language_);
      auto iter = types_map.find(name);
      if (iter == types_map.end()) {
        return error::Internal("Unrecognized base type: $0", name);
      }
      return iter->second;
    }
    case VarType::kPointer:
      return ir::shared::ScalarType::VOID_POINTER;
    case VarType::kStruct:
      if (language_ == ir::shared::Language::GOLANG) {
        if (name == "string") {
          return ir::shared::ScalarType::STRING;
        }
        if (name == "[]uint8" || name == "[]byte") {
          return ir::shared::ScalarType::BYTE_ARRAY;
        }
      }
      return error::Internal("Unhandled type: $0 (name=$1)", magic_enum::enum_name(type), name);
    default:
      return error::Internal("Unhandled type: $0 (name=$1)", magic_enum::enum_name(type), name);
  }
}

StatusOr<const ArgInfo*> GetArgInfo(const std::map<std::string, ArgInfo>& args_map,
                                    std::string_view arg_name) {
  auto args_map_iter = args_map.find(std::string(arg_name));
  if (args_map_iter == args_map.end()) {
    return error::Internal("Could not find argument $0", arg_name);
  }
  return &args_map_iter->second;
}

ir::physical::ScalarVariable* Dwarvifier::AddVariable(ir::physical::Probe* probe,
                                                      const std::string& name,
                                                      ir::shared::ScalarType type) {
  auto* var = probe->add_vars()->mutable_scalar_var();
  var->set_name(name);
  var->set_type(type);

  scalar_var_types_[name] = type;

  return var;
}

Status Dwarvifier::GenerateProbe(const ir::logical::Probe input_probe,
                                 ir::physical::Program* output_program) {
  PL_ASSIGN_OR_RETURN(args_map_,
                      dwarf_reader_->GetFunctionArgInfo(input_probe.trace_point().symbol()));
  PL_ASSIGN_OR_RETURN(retval_info_,
                      dwarf_reader_->GetFunctionRetValInfo(input_probe.trace_point().symbol()));

  scalar_var_types_.clear();

  PL_RETURN_IF_ERROR(ProcessProbe(input_probe, output_program));
  return Status::OK();
}

Status Dwarvifier::Setup(const ir::shared::DeploymentSpec& deployment_spec,
                         ir::shared::Language language) {
  using dwarf_tools::DwarfReader;

  PL_ASSIGN_OR_RETURN(dwarf_reader_, DwarfReader::Create(deployment_spec.path()));

  language_ = language;

  implicit_columns_ = {kTGIDVarName, kTGIDStartTimeVarName, kKTimeVarName};
  if (language_ == ir::shared::Language::GOLANG) {
    implicit_columns_.push_back(kGOIDVarName);
  }

  return Status::OK();
}

Status Dwarvifier::ProcessTracepoint(const ir::shared::TracePoint& trace_point,
                                     ir::physical::Probe* output_probe) {
  auto* probe_trace_point = output_probe->mutable_trace_point();
  probe_trace_point->CopyFrom(trace_point);
  probe_trace_point->set_type(trace_point.type());

  return Status::OK();
}

namespace {

bool IsFunctionLatecySpecified(const ir::logical::Probe& probe) {
  return probe.function_latency_oneof_case() ==
         ir::logical::Probe::FunctionLatencyOneofCase::kFunctionLatency;
}

}  // namespace

Status Dwarvifier::ProcessProbe(const ir::logical::Probe& input_probe,
                                ir::physical::Program* output_program) {
  auto* p = output_program->add_probes();

  p->set_name(input_probe.name());

  PL_RETURN_IF_ERROR(ProcessTracepoint(input_probe.trace_point(), p));
  PL_RETURN_IF_ERROR(AddSpecialVariables(p));

  for (auto& constant : input_probe.consts()) {
    PL_RETURN_IF_ERROR(ProcessConstants(constant, p));
  }

  for (const auto& arg : input_probe.args()) {
    PL_RETURN_IF_ERROR(ProcessArgExpr(arg, p));
  }

  for (const auto& ret_val : input_probe.ret_vals()) {
    PL_RETURN_IF_ERROR(ProcessRetValExpr(ret_val, p));
  }

  for (const auto& map_val : input_probe.map_vals()) {
    PL_RETURN_IF_ERROR(ProcessMapVal(map_val, p));
  }

  if (IsFunctionLatecySpecified(input_probe)) {
    PL_RETURN_IF_ERROR(ProcessFunctionLatency(input_probe.function_latency(), p));
  }

  for (const auto& stash_action : input_probe.map_stash_actions()) {
    PL_RETURN_IF_ERROR(ProcessStashAction(stash_action, p, output_program));
  }

  for (const auto& delete_action : input_probe.map_delete_actions()) {
    PL_RETURN_IF_ERROR(ProcessDeleteAction(delete_action, p));
  }

  for (const auto& output_action : input_probe.output_actions()) {
    PL_RETURN_IF_ERROR(ProcessOutputAction(output_action, p, output_program));
  }

  for (const auto& printk : input_probe.printks()) {
    p->add_printks()->CopyFrom(printk);
  }

  return Status::OK();
}

Status Dwarvifier::AddSpecialVariables(ir::physical::Probe* output_probe) {
  PL_RETURN_IF_ERROR(AddStandardVariables(output_probe));

  if (output_probe->trace_point().type() == ir::shared::TracePoint::RETURN) {
    PL_RETURN_IF_ERROR(AddRetProbeVariables(output_probe));
  }

  return Status::OK();
}

// TODO(oazizi): Could selectively generate some of these variables, when they are not required.
//               For example, if latency is not required, then there is no need for ktime.
//               For now, include them all for simplicity.
Status Dwarvifier::AddStandardVariables(ir::physical::Probe* output_probe) {
  // Add SP variable.
  auto* sp_var = AddVariable(output_probe, kSPVarName, ir::shared::VOID_POINTER);
  sp_var->set_reg(ir::physical::Register::SP);

  // Add tgid variable.
  auto* tgid_var = AddVariable(output_probe, kTGIDVarName, ir::shared::ScalarType::INT32);
  tgid_var->set_builtin(ir::shared::BPFHelper::TGID);

  // Add tgid_pid variable.
  auto* tgid_pid_var = AddVariable(output_probe, kTGIDPIDVarName, ir::shared::ScalarType::UINT64);
  tgid_pid_var->set_builtin(ir::shared::BPFHelper::TGID_PID);

  // Add TGID start time (required for UPID construction).
  auto* tgid_start_time_var =
      AddVariable(output_probe, kTGIDStartTimeVarName, ir::shared::ScalarType::UINT64);
  tgid_start_time_var->set_builtin(ir::shared::BPFHelper::TGID_START_TIME);

  // Add current time variable (for latency).
  auto* ktime_var = AddVariable(output_probe, kKTimeVarName, ir::shared::ScalarType::UINT64);
  ktime_var->set_builtin(ir::shared::BPFHelper::KTIME);

  // Add goid variable (if this is a go binary).
  if (language_ == ir::shared::Language::GOLANG) {
    auto* goid_var = AddVariable(output_probe, kGOIDVarName, ir::shared::ScalarType::INT64);
    goid_var->set_builtin(ir::shared::BPFHelper::GOID);
  }

  return Status::OK();
}

Status Dwarvifier::AddRetProbeVariables(ir::physical::Probe* output_probe) {
  // Add return value variable for convenience.
  if ((language_ == ir::shared::C || language_ == ir::shared::CPP)) {
    auto* rc_var = AddVariable(output_probe, kRCVarName, ir::shared::VOID_POINTER);
    rc_var->set_reg(ir::physical::Register::RC);
  }

  return Status::OK();
}

Status Dwarvifier::ProcessConstants(const ir::logical::Constant& constant,
                                    ir::physical::Probe* output_probe) {
  auto* var = output_probe->add_vars()->mutable_scalar_var();

  var->set_name(constant.name());
  var->set_type(constant.type());
  var->set_constant(constant.constant());

  return Status::OK();
}

Status Dwarvifier::ProcessVarExpr(const std::string& var_name, const ArgInfo* arg_info,
                                  const std::string& base_var,
                                  const std::vector<std::string_view>& components,
                                  ir::physical::Probe* output_probe) {
  VarType type = arg_info->type;
  std::string type_name = std::move(arg_info->type_name);
  int offset = kSPOffset + arg_info->offset;
  std::string base = base_var;
  std::string name = var_name;

  // Note that we start processing at element [1], not [0], which was used to set the starting
  // state in the lines above.
  for (auto iter = components.begin() + 1; iter < components.end(); ++iter) {
    // If parent is a pointer, create a variable to dereference it.
    if (type == VarType::kPointer) {
      PL_ASSIGN_OR_RETURN(ir::shared::ScalarType pb_type,
                          VarTypeToProtoScalarType(type, type_name));

      absl::StrAppend(&name, kDerefStr);

      auto* var = AddVariable(output_probe, name, pb_type);
      var->mutable_memory()->set_base(base);
      var->mutable_memory()->set_offset(offset);

      // Reset base and offset.
      base = name;
      offset = 0;
    }

    std::string_view field_name = *iter;
    PL_ASSIGN_OR_RETURN(VarInfo member_info,
                        dwarf_reader_->GetStructMemberInfo(type_name, field_name));
    offset += member_info.offset;
    type_name = std::move(member_info.type_name);
    type = member_info.type;
    absl::StrAppend(&name, kDotStr, field_name);
  }

  // If parent is a pointer, create a variable to dereference it.
  if (type == VarType::kPointer) {
    PL_ASSIGN_OR_RETURN(ir::shared::ScalarType pb_type, VarTypeToProtoScalarType(type, type_name));

    absl::StrAppend(&name, kDerefStr);

    auto* var = AddVariable(output_probe, name, pb_type);
    var->mutable_memory()->set_base(base);
    var->mutable_memory()->set_offset(offset);

    // Reset base and offset.
    base = name;
    offset = 0;

    // Since we are the leaf, also force the type to a BaseType.
    // If we are not, in fact, at a base type, then VarTypeToProtoScalarType will error out,
    // as it should--since we can't trace non-base types.
    type = VarType::kBaseType;
    absl::StrAppend(&name, kDerefStr);
  }

  PL_ASSIGN_OR_RETURN(ir::shared::ScalarType pb_type, VarTypeToProtoScalarType(type, type_name));

  // The very last created variable uses the original id.
  // This is important so that references in the original probe are maintained.

  auto* var = AddVariable(output_probe, std::string(var_name), pb_type);

  var->mutable_memory()->set_base(base);
  var->mutable_memory()->set_offset(offset);

  return Status::OK();
}

Status Dwarvifier::ProcessArgExpr(const ir::logical::Argument& arg,
                                  ir::physical::Probe* output_probe) {
  if (arg.expr().empty()) {
    return error::InvalidArgument("Argument '$0' expression cannot be empty", arg.id());
  }

  std::vector<std::string_view> components = absl::StrSplit(arg.expr(), ".");

  PL_ASSIGN_OR_RETURN(const ArgInfo* arg_info, GetArgInfo(args_map_, components.front()));
  DCHECK(arg_info != nullptr);

  return ProcessVarExpr(arg.id(), arg_info, kSPVarName, components, output_probe);
}

Status Dwarvifier::ProcessRetValExpr(const ir::logical::ReturnValue& ret_val,
                                     ir::physical::Probe* output_probe) {
  if (ret_val.expr().empty()) {
    return error::InvalidArgument("ReturnValue '$0' expression cannot be empty", ret_val.id());
  }

  std::vector<std::string_view> components = absl::StrSplit(ret_val.expr(), ".");

  int index = -1;

  if (!absl::SimpleAtoi(components.front().substr(1), &index)) {
    return error::InvalidArgument(
        "ReturnValue '$0' expression invalid, first component must be `$$<index>`", ret_val.expr());
  }

  switch (language_) {
    case ir::shared::GOLANG: {
      // TODO(oazizi): Support named return variables.
      // Golang automatically names return variables ~r0, ~r1, etc.
      // However, it should be noted that the indexing includes function arguments.
      // For example Foo(a int, b int) (int, int) would have ~r2 and ~r3 as return variables.
      // One additional nuance is that the receiver, although an argument for dwarf purposes,
      // is not counted in the indexing.
      // For now, we throw the burden of finding the index to the user,
      // so if they want the first return argument above, they would have to specify and index of 2.
      // TODO(oazizi): Make indexing of return value based on number of return arguments only.
      std::string ret_val_name = absl::StrCat("~r", std::to_string(index));

      // Reset the first component.
      components.front() = ret_val_name;

      // Golang return values are really arguments located on the stack, so get the arg info.
      PL_ASSIGN_OR_RETURN(const ArgInfo* arg_info, GetArgInfo(args_map_, components.front()));
      DCHECK(arg_info != nullptr);

      return ProcessVarExpr(ret_val.id(), arg_info, kSPVarName, components, output_probe);
    }
    case ir::shared::CPP:
    case ir::shared::C: {
      if (index != 0) {
        return error::Internal("C/C++ only supports a single return value [index=$0].", index);
      }

      switch (retval_info_.type) {
        case VarType::kBaseType: {
          // When the return value is a simple base type, the return value is passed directly
          // through a register that is accessed via PT_REGS_RC.
          PL_ASSIGN_OR_RETURN(ir::shared::ScalarType pb_type,
                              VarTypeToProtoScalarType(retval_info_.type, retval_info_.type_name));

          auto* rc_var = AddVariable(output_probe, ret_val.id(), pb_type);
          rc_var->set_reg(ir::physical::Register::RC);

          return Status::OK();
        }
        case VarType::kPointer: {
          // When the return value is not a simple base type, the return value is a pointer to the
          // struct. That pointer is accessed via PT_REGS_RC.

          // TODO(oazizi): Finish this code and create a test.

          ArgInfo retval;
          retval.type_name = retval_info_.type_name;
          retval.type = retval_info_.type;
          retval.offset = 0;

          return ProcessVarExpr(ret_val.id(), &retval, kRCVarName, components, output_probe);
        }
        case VarType::kVoid: {
          return error::Internal(
              "Attempting to process return variable for function with void return.",
              output_probe->trace_point().symbol());
        }
        default: {
          return error::Internal("Unexpected var type:", magic_enum::enum_name(retval_info_.type));
        }
      }
    }
    default:
      return error::Internal("Return expressions not yet supported for lanuage=$0",
                             magic_enum::enum_name(language_));
  }
}

Status Dwarvifier::ProcessMapVal(const ir::logical::MapValue& map_val,
                                 ir::physical::Probe* output_probe) {
  // Find the map.
  auto map_iter = maps_.find(map_val.map_name());
  if (map_iter == maps_.end()) {
    return error::Internal("ProcessMapVal [probe=$0]: Reference to undeclared map: $1",
                           output_probe->name(), map_val.map_name());
  }
  auto* map = map_iter->second;

  // Find the map struct.
  std::string struct_type_name = StructTypeName(map_val.map_name());
  auto struct_iter = structs_.find(struct_type_name);
  if (struct_iter == structs_.end()) {
    return error::Internal("ProcessMapVal [probe=$0]: Reference to undeclared struct: $1",
                           output_probe->name(), struct_type_name);
  }
  auto* struct_decl = struct_iter->second;

  std::string map_var_name = absl::StrCat(map_val.map_name(), "_ptr");

  // Create the map variable.
  {
    auto* var = output_probe->add_vars()->mutable_map_var();
    var->set_name(map_var_name);
    var->set_type(map->value_type().struct_type());
    var->set_map_name(map_val.map_name());
    PL_ASSIGN_OR_RETURN(std::string key_var_name, BPFHelperVariableName(map_val.key()));
    var->set_key_variable_name(key_var_name);
  }

  // Unpack the map variable's members.
  int i = 0;
  for (const auto& value_id : map_val.value_ids()) {
    const auto& field = struct_decl->fields(i++);

    auto* var = output_probe->add_vars()->mutable_member_var();
    var->set_name(value_id);
    var->set_type(field.type());
    var->set_struct_base(map_var_name);
    var->set_is_struct_base_pointer(true);
    var->set_field(field.name());

    scalar_var_types_[value_id] = field.type();
  }

  return Status::OK();
}

Status Dwarvifier::ProcessFunctionLatency(const ir::shared::FunctionLatency& function_latency,
                                          ir::physical::Probe* output_probe) {
  auto* var = output_probe->add_vars()->mutable_scalar_var();

  const auto& var_name = function_latency.id();

  var->set_name(var_name);
  var->set_type(ir::shared::ScalarType::INT64);

  auto* expr = var->mutable_binary_expr();

  expr->set_op(ir::physical::ScalarVariable::BinaryExpression::SUB);
  expr->set_lhs(kKTimeVarName);
  expr->set_rhs(kStartKTimeNSVarName);

  scalar_var_types_[var_name] = ir::shared::ScalarType::INT64;

  output_probe->mutable_function_latency()->CopyFrom(function_latency);

  // TODO(yzhao): Add more checks.

  return Status::OK();
}

Status Dwarvifier::GenerateMapValueStruct(const ir::logical::MapStashAction& stash_action_in,
                                          const std::string& struct_type_name,
                                          ir::physical::Program* output_program) {
  // TODO(oazizi): Check if struct already exists. If it does, make sure it is the same.

  auto* struct_decl = output_program->add_structs();
  struct_decl->set_name(struct_type_name);

  for (const auto& f : stash_action_in.value_variable_name()) {
    auto* struct_field = struct_decl->add_fields();
    struct_field->set_name(f);

    auto iter = scalar_var_types_.find(f);
    if (iter == scalar_var_types_.end()) {
      return error::Internal(
          "GenerateMapValueStruct [map_name=$0]: Reference to unknown variable: $1",
          stash_action_in.map_name(), f);
    }
    struct_field->set_type(iter->second);
  }

  structs_[struct_type_name] = struct_decl;
  return Status::OK();
}

namespace {
Status PopulateMapTypes(const std::map<std::string, ir::shared::Map*>& maps,
                        const std::string& map_name, const std::string& struct_type_name) {
  auto iter = maps.find(map_name);
  if (iter == maps.end()) {
    return error::Internal("Reference to undeclared map: $0", map_name);
  }

  auto* map = iter->second;

  // TODO(oazizi): Check if values are already set. If they are check for consistency.
  map->mutable_key_type()->set_scalar(ir::shared::ScalarType::UINT64);
  map->mutable_value_type()->set_struct_type(struct_type_name);

  return Status::OK();
}

}  // namespace

Status Dwarvifier::ProcessStashAction(const ir::logical::MapStashAction& stash_action_in,
                                      ir::physical::Probe* output_probe,
                                      ir::physical::Program* output_program) {
  std::string variable_name = stash_action_in.map_name() + "_value";
  std::string struct_type_name = StructTypeName(stash_action_in.map_name());

  PL_RETURN_IF_ERROR(GenerateMapValueStruct(stash_action_in, struct_type_name, output_program));
  PL_RETURN_IF_ERROR(PopulateMapTypes(maps_, stash_action_in.map_name(), struct_type_name));

  auto* struct_var = output_probe->add_vars()->mutable_struct_var();
  struct_var->set_name(variable_name);
  struct_var->set_type(struct_type_name);

  for (const auto& f : stash_action_in.value_variable_name()) {
    auto* fa = struct_var->add_field_assignments();
    fa->set_field_name(f);
    fa->set_variable_name(f);
  }

  auto* stash_action_out = output_probe->add_map_stash_actions();
  stash_action_out->set_map_name(stash_action_in.map_name());

  PL_ASSIGN_OR_RETURN(std::string key_var_name, BPFHelperVariableName(stash_action_in.key()));
  stash_action_out->set_key_variable_name(key_var_name);
  stash_action_out->set_value_variable_name(variable_name);
  stash_action_out->mutable_cond()->CopyFrom(stash_action_in.cond());

  return Status::OK();
}

Status Dwarvifier::ProcessDeleteAction(const ir::logical::MapDeleteAction& delete_action_in,
                                       ir::physical::Probe* output_probe) {
  auto* delete_action_out = output_probe->add_map_delete_actions();
  delete_action_out->set_map_name(delete_action_in.map_name());

  PL_ASSIGN_OR_RETURN(std::string key_var_name, BPFHelperVariableName(delete_action_in.key()));
  delete_action_out->set_key_variable_name(key_var_name);

  return Status::OK();
}

Status Dwarvifier::GenerateOutputStruct(const ir::logical::OutputAction& output_action_in,
                                        const std::string& struct_type_name,
                                        ir::physical::Program* output_program) {
  // TODO(oazizi): Check if struct already exists. If it does, make sure it is the same.

  auto* struct_decl = output_program->add_structs();
  struct_decl->set_name(struct_type_name);

  for (const auto& f : implicit_columns_) {
    auto* struct_field = struct_decl->add_fields();
    struct_field->set_name(f);

    auto iter = scalar_var_types_.find(f);
    if (iter == scalar_var_types_.end()) {
      return error::Internal("GenerateOutputStruct [output=$0]: Reference to unknown variable $1",
                             output_action_in.output_name(), f);
    }

    struct_field->set_type(iter->second);
  }

  auto output_iter = outputs_.find(output_action_in.output_name());

  if (output_iter == outputs_.end()) {
    return error::InvalidArgument("Output '$0' was not defined", output_action_in.output_name());
  }

  const ir::physical::PerfBufferOutput* output = output_iter->second;

  if (output->fields_size() != output_action_in.variable_name_size()) {
    return error::InvalidArgument(
        "OutputAction to '$0' writes $1 variables, but the Output has $2 fields",
        output_action_in.output_name(), output_action_in.variable_name_size(),
        output->fields_size());
  }

  for (int i = 0; i < output_action_in.variable_name_size(); ++i) {
    auto* struct_field = struct_decl->add_fields();

    // Set the field name to the name in the Output.
    struct_field->set_name(output->fields(i));

    const std::string& var_name = output_action_in.variable_name(i);

    auto iter = scalar_var_types_.find(var_name);
    if (iter == scalar_var_types_.end()) {
      return error::Internal("GenerateOutputStruct [output=$0]: Reference to unknown variable $1",
                             output_action_in.output_name(), var_name);
    }
    struct_field->set_type(iter->second);
  }

  structs_[struct_type_name] = struct_decl;
  return Status::OK();
}

namespace {
Status PopulateOutputTypes(const std::map<std::string, ir::physical::PerfBufferOutput*>& outputs,
                           const std::string& output_name, const std::string& struct_type_name) {
  auto iter = outputs.find(output_name);
  if (iter == outputs.end()) {
    return error::Internal("Reference to undeclared map: $0", output_name);
  }

  auto* output = iter->second;

  if (!output->struct_type().empty() && output->struct_type() != struct_type_name) {
    return error::InvalidArgument("Output '$0' has output type '$1', which should be '$2'",
                                  output_name, output->struct_type(), struct_type_name);
  }

  output->set_struct_type(struct_type_name);

  return Status::OK();
}
}  // namespace

Status Dwarvifier::ProcessOutputAction(const ir::logical::OutputAction& output_action_in,
                                       ir::physical::Probe* output_probe,
                                       ir::physical::Program* output_program) {
  std::string variable_name = output_action_in.output_name() + "_value";
  std::string struct_type_name = StructTypeName(output_action_in.output_name());

  // Generate struct definition.
  PL_RETURN_IF_ERROR(GenerateOutputStruct(output_action_in, struct_type_name, output_program));

  // Generate an output definition.
  PL_RETURN_IF_ERROR(
      PopulateOutputTypes(outputs_, output_action_in.output_name(), struct_type_name));

  // Create and initialize a struct variable.
  auto* struct_var = output_probe->add_vars()->mutable_struct_var();
  struct_var->set_type(struct_type_name);
  struct_var->set_name(variable_name);

  // The Struct generated in above step is always the last element.
  const ir::physical::Struct& output_struct = *output_program->structs().rbegin();
  int struct_field_index = 0;

  for (const auto& f : implicit_columns_) {
    auto* fa = struct_var->add_field_assignments();
    fa->set_field_name(output_struct.fields(struct_field_index++).name());
    fa->set_variable_name(f);
  }

  for (const auto& f : output_action_in.variable_name()) {
    auto* fa = struct_var->add_field_assignments();
    fa->set_field_name(output_struct.fields(struct_field_index++).name());
    fa->set_variable_name(f);
  }

  // Output data.
  auto* output_action_out = output_probe->add_output_actions();
  output_action_out->set_perf_buffer_name(output_action_in.output_name());
  output_action_out->set_variable_name(variable_name);

  return Status::OK();
}

}  // namespace dynamic_tracing
}  // namespace stirling
}  // namespace pl
