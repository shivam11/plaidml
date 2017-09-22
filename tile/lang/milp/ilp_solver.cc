#include "tile/lang/milp/ilp_solver.h"

namespace vertexai {
namespace tile {
namespace lang {
namespace milp {

std::map<std::string, Rational> ILPSolver::reportSolution() const {
  std::vector<Rational> sym_soln = getSymbolicSolution();
  std::map<std::string, Rational> soln;
  for (size_t i = 0; i < sym_soln.size(); ++i) {
    soln[var_names_[i]] = sym_soln[i];
  }
  return soln;
}

std::vector<ILPResult> ILPSolver::batch_solve(const std::vector<RangeConstraint>& constraints,
                                              const std::vector<Polynomial>& objectives) {
  // Solve a batch of ILP problems, all with the same constraints but different objectives
  Tableau t = makeStandardFormTableau(constraints);
  if (!t.convertToCanonicalForm()) {
    throw std::runtime_error("Unable to run ILPSolver::batch_solve: Feasible region empty.");
  }
  var_names_ = t.varNames();
  t.convertToCanonicalForm();

  std::vector<ILPResult> ret;
  for (const Polynomial& obj : objectives) {
    clean();
    var_names_ = t.varNames();

    // Copy tableau for manipulation specific to the objective
    Tableau specific_t = t;

    // Set first row based on objective
    specific_t.mat()(0, 0) = 1;
    for (size_t i = 0; i < t.varNames().size(); ++i) {
      std::string var = t.varNames()[i];
      if (var.substr(var.size() - 4, 4) == "_pos") {
        specific_t.mat()(0, i + 1) = -obj[var.substr(0, var.size() - 4)];
      } else if (var.substr(var.size() - 4, 4) == "_neg") {
        specific_t.mat()(0, i + 1) = obj[var.substr(0, var.size() - 4)];
      } else {
        // Do nothing: We're on a slack variable or other artificially added variable
      }
    }

    // Since objective was reset, need to price out to make canonical
    specific_t.priceOut();
    if (!solve(specific_t, true)) {
      throw std::runtime_error("Feasible region has empty intersection with integers.");
    }
    ret.emplace_back(obj, reportObjective(), reportSolution());
  }
  return ret;
}

bool ILPSolver::solve(const std::vector<RangeConstraint>& constraints, const Polynomial objective) {
  if (VLOG_IS_ON(1)) {
    std::ostringstream msg;
    msg << "Starting ILPSolver with constraints\n";
    for (const RangeConstraint& c : constraints) {
      msg << "  " << c << "\n";
    }
    msg << "and objective " << objective;
    IVLOG(1, msg.str());
  }
  Tableau t = makeStandardFormTableau(constraints, objective);
  return solve(t);
}

bool ILPSolver::solve(Tableau& tableau, bool already_canonical) {
  clean();
  var_names_ = tableau.varNames();
  IVLOG(4, "Starting ILPSolver with tableau " << tableau.mat().toString());
  solve_step(tableau, already_canonical);
  return feasible_found;
}

void ILPSolver::solve_step(Tableau& tableau, bool already_canonical) {
  // Check feasible region exists for this subproblem
  if (!tableau.makeOptimal(already_canonical)) {
    // Feasible region empty (or unbounded), no solution from this branch
    IVLOG(4, "Feasible region empty; pruning branch");
    return;
  }

  // Check if LP relaxation objective is better than current best found ILP objective
  Rational obj_val = tableau.reportObjectiveValue();
  if (obj_val >= best_objective && feasible_found) {
    // Best real solution of this subproblem is worse than best overall int solution
    // found so far, so prune this branch
    IVLOG(4, "Objective value " << obj_val << " proven suboptimal; pruning branch");
    return;
  }

  // Check if this solution is integral
  std::vector<Rational> soln = tableau.getSymbolicSolution();
  // Infeasibleness is in [0, 1/2] (frac part dist from 1/2), with less being more infeasible
  Rational most_infeasible_infeas_value = 1;
  Rational most_infeasible_full_value = 0;
  size_t most_infeasible_variable = 0;  // 0 is not a valid variable, so can use as "none"
  for (size_t i = 0; i < soln.size(); ++i) {
    Rational frac_part = soln[i] - Floor(soln[i]);
    if (frac_part != 0) {
      Rational infeasibleness = Abs(frac_part - Rational(1, 2));
      if (infeasibleness < most_infeasible_infeas_value) {
        most_infeasible_infeas_value = infeasibleness;
        most_infeasible_variable = i + 1;  // TODO(T1146): Keep eye on possible off-by-one error
        most_infeasible_full_value = soln[i];
      }
    }
  }
  if (most_infeasible_variable == 0) {
    // This is an integer solution better than any previous!
    if (VLOG_IS_ON(3)) {
      std::ostringstream msg;
      msg << "Found new best integer solution!"
          << "  objective: " << obj_val << "\n";
      msg << "  Solution is:";
      for (size_t i = 0; i < soln.size(); ++i) {
        msg << "\n    " << tableau.varNames()[i] << ": " << soln[i];
      }
      IVLOG(4, msg.str());
      IVLOG(6, "  from tableau:" << tableau.mat().toString());
    }
    feasible_found = true;
    best_objective = obj_val;
    best_solution = soln;
    return;
  } else {
    // This is a non-integer solution; branch
    if (VLOG_IS_ON(5)) {
      std::ostringstream msg;
      msg << "Found non-integer solution;"
          << "  objective: " << obj_val << "\n";
      msg << "  Solution is:";
      for (size_t i = 0; i < soln.size(); ++i) {
        msg << "\n    " << tableau.varNames()[i] << ": " << soln[i];
      }
      IVLOG(5, msg.str());
      IVLOG(6, "  from tableau:" << tableau.mat().toString());
    }

    // TODO(T1146): The following *should* be redundant w/ "most_infeasible_variable", but apparently isn't?
    Rational greatest_fractional = 0;
    size_t greatest_fractional_row = 0;
    for (size_t i = 1; i < tableau.mat().size1(); ++i) {
      Rational frac = tableau.mat()(i, tableau.mat().size2() - 1) - Floor(tableau.mat()(i, tableau.mat().size2() - 1));
      if (frac > greatest_fractional) {
        greatest_fractional = frac;
        greatest_fractional_row = i;
      }
    }

    IVLOG(3, "Requesting Gomory cut at row " << greatest_fractional_row << " with value " << greatest_fractional);
    Tableau with_cut = addGomoryCut(tableau, greatest_fractional_row);
    IVLOG(5, "Adding Gomory cut yielded: " << with_cut.mat().toString());
    solve_step(with_cut);
  }
}

Tableau ILPSolver::addGomoryCut(const Tableau& t, size_t row) {
  IVLOG(5, "Adding Gomory cut along row " << row);
  Tableau ret(t.mat().size1() + 1, t.mat().size2() + 1, t.varNames(), &t.getOpposites());
  project(ret.mat(), range(0, t.mat().size1()), range(0, t.mat().size2() - 1)) =
      project(t.mat(), range(0, t.mat().size1()), range(0, t.mat().size2() - 1));
  project(ret.mat(), range(0, t.mat().size1()), range(t.mat().size2(), t.mat().size2() + 1)) =
      project(t.mat(), range(0, t.mat().size1()), range(t.mat().size2() - 1, t.mat().size2()));
  // Note: Assumes the uninitialized column was set to all 0s, which appears to
  // be an undocumented feature of ublas.
  for (size_t j = 0; j < t.mat().size2() - 1; ++j) {
    ret.mat()(t.mat().size1(), j) = t.mat()(row, j) - Floor(t.mat()(row, j));
  }
  ret.mat()(t.mat().size1(), t.mat().size2() - 1) = -1;
  ret.mat()(t.mat().size1(), t.mat().size2()) =
      t.mat()(row, t.mat().size2() - 1) - Floor(t.mat()(row, t.mat().size2() - 1));
  return ret;
}

Tableau ILPSolver::makeStandardFormTableau(const std::vector<RangeConstraint>& constraints,
                                           const Polynomial objective) {
  // Create the standard form linear program for minimizing objective subject to the given constraints

  // TODO(T1146): Choose names for the slack variables in a way that guarantees no
  // conflict with any already existing variable names.
  // TODO(T1146): Also choose names for the positive and negative part variables
  // to also ensure no variable name conflicts.

  std::vector<Polynomial> lp_constraints;  // The represented constraint is poly == 0
  unsigned int slack_count = 0;

  std::vector<std::string> var_names;  // Ordered list of variable names used in this Tableau

  // var_index indicates what column in the Tableau will go with the variable name
  // Note that these are indexed from 0 but have 1 as the smallest value as the first
  // column in the Tableau is for the objective and does not have a variable name
  std::map<std::string, size_t> var_index;
  for (const RangeConstraint& c : constraints) {
    Polynomial poly(c.poly);

    // Split each variable into + and - parts
    // First extract keys (i.e. var names)
    std::vector<std::string> local_vars;
    for (const auto& kvp : poly.getMap()) {
      const std::string& key = kvp.first;
      if (key != "") {  // Do nothing for constant term
        local_vars.emplace_back(key);
      }
    }

    // Replace each var with + and - parts
    for (const std::string& var : local_vars) {
      std::map<std::string, size_t>::iterator unused;
      bool added_new_var;
      poly.substitute(var, Polynomial(var + "_pos") - Polynomial(var + "_neg"));
      std::tie(unused, added_new_var) = var_index.emplace(var + "_pos", var_index.size() + 1);
      if (added_new_var) {
        var_names.emplace_back(var + "_pos");
      }
      std::tie(unused, added_new_var) = var_index.emplace(var + "_neg", var_index.size() + 1);
      if (added_new_var) {
        var_names.emplace_back(var + "_neg");
      }
    }

    // Make LP constraint from lower bound
    std::string slack_var = "slack" + std::to_string(slack_count);
    lp_constraints.emplace_back(poly - Polynomial(slack_var));
    var_names.emplace_back(slack_var);
    var_index.emplace(slack_var, var_index.size() + 1);
    ++slack_count;

    // Make LP constraint from upper bound
    slack_var = "slack" + std::to_string(slack_count);
    lp_constraints.emplace_back(poly + Polynomial(slack_var) - c.range + 1);
    var_names.emplace_back(slack_var);
    var_index.emplace(slack_var, var_index.size() + 1);
    ++slack_count;
  }

  // The tableau has a row for each lp_constraint plus a row for the objective, and a
  // column for each variable plus a column for the constant terms and a column for the objective.
  Tableau tableau(lp_constraints.size() + 1, var_index.size() + 2, var_names);

  // Put the data in the Tableau
  // First the objective:
  tableau.mat()(0, 0) = 1;
  for (const auto& kvp : objective.getMap()) {
    if (kvp.first != "") {
      // The positive and negative parts have reversed sign because the algorithm
      // needs to use -objective for the coeffs of the first row
      try {
        tableau.mat()(0, var_index.at(kvp.first + "_pos")) = -kvp.second;
        tableau.mat()(0, var_index.at(kvp.first + "_neg")) = kvp.second;
      } catch (const std::out_of_range& e) {
        throw std::out_of_range("Bad index given to Tableau objective: " + kvp.first);
      }
    }
  }

  // Now the constraints:
  size_t constraint_idx = 1;  // Start from 1 b/c the first row is for the object
  for (const Polynomial& poly : lp_constraints) {
    short const_sign = 1;  // NOLINT (runtime/int)
    if (poly.constant() <= 0) {
      const_sign = -1;  // Last column must be positive, so negate everything if const term is positive
    }
    for (const auto& kvp : poly.getMap()) {
      if (kvp.first == "") {
        // The negative of the constant term goes in the last column of the tableau
        tableau.mat()(constraint_idx, tableau.mat().size2() - 1) = -const_sign * -kvp.second;
      } else {
        tableau.mat()(constraint_idx, var_index.at(kvp.first)) = -const_sign * kvp.second;
      }
    }
    ++constraint_idx;
  }

  return tableau;
}

void ILPSolver::clean() {
  feasible_found = false;
  best_objective = 0;
  best_solution.clear();
  var_names_.clear();
}
}  // namespace milp
}  // namespace lang
}  // namespace tile
}  // namespace vertexai