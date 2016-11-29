/*******************************************************************************
The MIT License (MIT)

Copyright (c) 2015 Dmitry "Dima" Korolev <dmitry.korolev@gmail.com>
          (c) 2015 Maxim Zhurovich <zhurovich@gmail.com>

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*******************************************************************************/

#ifndef FNCAS_OPTIMIZE_H
#define FNCAS_OPTIMIZE_H

#include <algorithm>
#include <numeric>
#include <map>
#include <string>

#include "base.h"
#include "differentiate.h"
#include "exceptions.h"
#include "logger.h"
#include "mathutil.h"
#include "node.h"

#ifdef FNCAS_JIT
#include "jit.h"
#endif  // FNCAS_JIT

#include "../../Bricks/template/decay.h"
#include "../../TypeSystem/struct.h"
#include "../../TypeSystem/optional.h"
#include "../../TypeSystem/helpers.h"

namespace fncas {

// clang-format off
CURRENT_STRUCT(OptimizationResult, ValueAndPoint) {
  CURRENT_CONSTRUCTOR(OptimizationResult)(const ValueAndPoint& p) : SUPER(p) {}
};
// clang-format on

enum class EarlyStoppingCriterion : bool { StopOptimization = false, ContinueOptimization = true };

class OptimizerParameters {
 public:
  using point_beautifier_t = std::function<std::string(const std::vector<double_t>& x)>;
  using stopping_criterion_t =
      std::function<EarlyStoppingCriterion(size_t iterations_done, const std::vector<double_t>& x)>;

  template <typename T>
  OptimizerParameters& SetValue(std::string name, T value) {
    static_assert(std::is_arithmetic<T>::value, "Value must be numeric");
    params_[name] = value;
    return *this;
  }

  template <typename T>
  const T GetValue(std::string name, T default_value) const {
    static_assert(std::is_arithmetic<T>::value, "Value must be numeric");
    if (params_.count(name)) {
      return static_cast<T>(params_.at(name));
    } else {
      return default_value;
    }
  }

  OptimizerParameters& SetPointBeautifier(point_beautifier_t point_beautifier) {
    point_beautifier_ = point_beautifier;
    return *this;
  }

  point_beautifier_t GetPointBeautifier() const { return point_beautifier_; }

  OptimizerParameters& SetStoppingCriterion(stopping_criterion_t stopping_criterion) {
    stopping_criterion_ = stopping_criterion;
    return *this;
  }

  stopping_criterion_t GetStoppingCriterion() const { return stopping_criterion_; }

 private:
  std::map<std::string, double_t> params_;
  point_beautifier_t point_beautifier_;
  stopping_criterion_t stopping_criterion_;
};

// The base class for the optimizer of the function of type `F`.
template <class F>
class Optimizer : noncopyable {
 public:
  virtual ~Optimizer() = default;

  Optimizer() : f_instance_(std::make_unique<F>()), f_reference_(*f_instance_) {}
  Optimizer(const OptimizerParameters& parameters)
      : f_instance_(std::make_unique<F>()), f_reference_(*f_instance_), parameters_(parameters) {}

  Optimizer(F& f) : f_reference_(f) {}
  Optimizer(const OptimizerParameters& parameters, F& f) : f_reference_(f), parameters_(parameters) {}

  template <typename ARG,
            class = std::enable_if_t<!std::is_same<current::decay<ARG>, OptimizerParameters>::value>,
            typename... ARGS>
  Optimizer(ARG&& arg, ARGS&&... args)
      : f_instance_(std::make_unique<F>(std::forward<ARG>(arg), std::forward<ARGS>(args)...)),
        f_reference_(*f_instance_) {}

  template <typename ARG, typename... ARGS>
  Optimizer(const OptimizerParameters& parameters, ARG&& arg, ARGS&&... args)
      : f_instance_(std::make_unique<F>(std::forward<ARG>(arg), std::forward<ARGS>(args)...)),
        f_reference_(*f_instance_),
        parameters_(parameters) {}

  F& Function() { return f_reference_; }
  const F& Function() const { return f_reference_; }
  F* operator->() { return &f_reference_; }
  const F* operator->() const { return &f_reference_; }

  const Optional<OptimizerParameters>& Parameters() const { return parameters_; }

  virtual OptimizationResult Optimize(const std::vector<double_t>& starting_point) const = 0;

  std::string PointAsString(const std::vector<double_t>& point) const {
    if (!Exists(parameters_) || !Value(parameters_).GetPointBeautifier()) {
      return JSON(std::vector<double>(point.begin(), point.end()));  // Must support other `double_t` types too. -- D.K.
    } else {
      return Value(parameters_).GetPointBeautifier()(point);
    }
  }

  EarlyStoppingCriterion StoppingCriterionSatisfied(size_t iterations_completed,
                                                    const std::vector<double_t>& point) const {
    if (!Exists(parameters_) || !Value(parameters_).GetStoppingCriterion()) {
      return EarlyStoppingCriterion::ContinueOptimization;
    } else {
      return Value(parameters_).GetStoppingCriterion()(iterations_completed, point);
    }
  }

 private:
  std::unique_ptr<F> f_instance_;             // The function to optimize: instance if owned by the optimizer.
  F& f_reference_;                            // The function to optimize: reference to work with, owned or not owned.
  Optional<OptimizerParameters> parameters_;  // Optimization parameters.
};

// The generic implementation running the ultimate algorithm on intermediate or compiled versions of function+gradient.
template <typename IMPL>
struct OptimizeImpl;

template <class F, class IMPL>
class OptimizeInvoker : public Optimizer<F> {
 public:
  using super_t = Optimizer<F>;
  using super_t::super_t;

  OptimizationResult Optimize(const std::vector<double_t>& starting_point) const override {
    const auto& logger = OptimizerLogger();

    const size_t dim = starting_point.size();
    const fncas::X gradient_helper(dim);
    const fncas::f_intermediate f_i(super_t::Function().ObjectiveFunction(gradient_helper));
    logger.Log("Optimizer: The objective function is " + current::ToString(node_vector_singleton().size()) + " nodes.");
#ifdef FNCAS_JIT
    logger.Log("Optimizer: Compiling the objective function.");
    const auto compile_f_begin_gradient = current::time::Now();
    fncas::f_compiled f = fncas::f_compiled(f_i);
    logger.Log("Optimizer: Done compiling the objective function, took " +
               current::ToString((current::time::Now() - compile_f_begin_gradient).count() * 1e-6) + " seconds.");
#else
    const auto& f = f_i;
#endif

    logger.Log("Optimizer: Differentiating.");
    const fncas::g_intermediate g_i(gradient_helper, f_i);
    logger.Log("Optimizer: Augmented with the gradient the function is " +
               current::ToString(node_vector_singleton().size()) + " nodes.");
#ifdef FNCAS_JIT
    logger.Log("Optimizer: Compiling the gradient.");
    const auto compile_g_begin_gradient = current::time::Now();
    fncas::g_compiled g = fncas::g_compiled(f_i, g_i);
    logger.Log("Optimizer: Done compiling the gradient, took " +
               current::ToString((current::time::Now() - compile_g_begin_gradient).count() * 1e-6) + " seconds.");
#else
    const auto& g = g_i;
#endif

    return OptimizeImpl<IMPL>::template RunOptimize<F>(*this, f, g, starting_point);
  }
};

// Naive gradient descent that tries 3 different step sizes in each iteration.
// Searches for a local minimum of `F::ObjectiveFunction` function.
struct GradientDescentOptimizerSelector;

template <class F>
class GradientDescentOptimizer final : public OptimizeInvoker<F, GradientDescentOptimizerSelector> {
 public:
  using super_t = OptimizeInvoker<F, GradientDescentOptimizerSelector>;
  using super_t::super_t;
};

template <>
struct OptimizeImpl<GradientDescentOptimizerSelector> {
  template <typename ORIGINAL_F, typename F, typename G>
  static OptimizationResult RunOptimize(const Optimizer<ORIGINAL_F>& super,
                                        F&& f,
                                        G&& g,
                                        const std::vector<double_t>& starting_point) {
    const auto& logger = OptimizerLogger();

    size_t max_steps = 2500;                             // Maximum number of optimization steps.
    double_t step_factor = 1.0;                          // Gradient is multiplied by this factor.
    double_t min_absolute_per_step_improvement = 1e-25;  // Terminate early if the absolute improvement is small.
    double_t min_relative_per_step_improvement = 1e-25;  // Terminate early if the relative improvement is small.
    double_t no_improvement_steps_to_terminate = 2;      // Wait for this # of consecutive no improvement iterations.

    if (Exists(super.Parameters())) {
      const auto& parameters = Value(super.Parameters());
      max_steps = parameters.GetValue("max_steps", max_steps);
      step_factor = parameters.GetValue("step_factor", step_factor);
      min_relative_per_step_improvement =
          parameters.GetValue("min_relative_per_step_improvement", min_relative_per_step_improvement);
      min_absolute_per_step_improvement =
          parameters.GetValue("min_absolute_per_step_improvement", min_absolute_per_step_improvement);
      no_improvement_steps_to_terminate =
          parameters.GetValue("no_improvement_steps_to_terminate", no_improvement_steps_to_terminate);
    }

    logger.Log("GradientDescentOptimizer: Begin at " + super.PointAsString(starting_point));

    ValueAndPoint current(f(starting_point), starting_point);

    size_t iteration;
    int no_improvement_steps = 0;
    {
      OptimizerStats stats("GradientDescentOptimizer");
      for (iteration = 0; iteration < max_steps; ++iteration) {
        if (super.StoppingCriterionSatisfied(iteration, current.point) == EarlyStoppingCriterion::StopOptimization) {
          logger.Log("GradientDescentOptimizer: External stopping criterion satisfied, terminating.");
          break;
        }

        stats.JournalIteration();
        if (logger) {
          // Expensive call, don't make it if `logger` is not initialized.
          logger.Log("GradientDescentOptimizer: Iteration " + current::ToString(iteration + 1) + ", OF = " +
                     current::ToString(current.value) + " @ " + super.PointAsString(current.point));
        }
        stats.JournalGradient();
        const auto gradient = g(current.point);
        auto best_candidate = current;
        auto has_valid_candidate = false;
        for (const double_t step : {0.01, 0.05, 0.2}) {  // TODO(dkorolev): Something more sophisticated maybe?
          const auto candidate_point(SumVectors(current.point, gradient, -step));
          stats.JournalFunction();
          const double_t value = f(candidate_point);
          if (fncas::IsNormal(value)) {
            has_valid_candidate = true;
            logger.Log("GradientDescentOptimizer: Value " + current::ToString(value) + " at step " +
                       current::ToString(step));
            best_candidate = std::min(best_candidate, ValueAndPoint(value, candidate_point));
          }
        }
        if (!has_valid_candidate) {
          CURRENT_THROW(FnCASOptimizationException("!fncas::IsNormal(value)"));
        }
        if (best_candidate.value / current.value > 1.0 - min_relative_per_step_improvement ||
            current.value - best_candidate.value < min_absolute_per_step_improvement) {
          ++no_improvement_steps;
          if (no_improvement_steps >= no_improvement_steps_to_terminate) {
            logger.Log("GradientDescentOptimizer: Terminating due to no improvement.");
            break;
          }
        } else {
          no_improvement_steps = 0;
        }
        current = best_candidate;
      }
    }
    logger.Log("GradientDescentOptimizer: Result = " + super.PointAsString(current.point));
    logger.Log("GradientDescentOptimizer: Objective function = " + current::ToString(current.value));

    return current;
  }
};

// Simple gradient descent optimizer with backtracking algorithm.
// Searches for a local minimum of `F::ObjectiveFunction` function.
struct GradientDescentOptimizerBTSelector;

template <class F>
class GradientDescentOptimizerBT final : public OptimizeInvoker<F, GradientDescentOptimizerBTSelector> {
 public:
  using super_t = OptimizeInvoker<F, GradientDescentOptimizerBTSelector>;
  using super_t::super_t;
};

template <>
struct OptimizeImpl<GradientDescentOptimizerBTSelector> {
  template <typename ORIGINAL_F, typename F, typename G>
  static OptimizationResult RunOptimize(const Optimizer<ORIGINAL_F>& super,
                                        F&& f,
                                        G&& g,
                                        const std::vector<double_t>& starting_point) {
    const auto& logger = OptimizerLogger();

    size_t min_steps = 3;       // Minimum number of optimization steps (ignoring early stopping).
    size_t max_steps = 250;     // Maximum number of optimization steps.
    double_t bt_alpha = 0.5;    // Alpha parameter for backtracking algorithm.
    double_t bt_beta = 0.8;     // Beta parameter for backtracking algorithm.
    size_t bt_max_steps = 100;  // Maximum number of backtracking steps.
    double_t grad_eps = 1e-8;   // Magnitude of gradient for early stopping.
    double_t min_absolute_per_step_improvement = 1e-25;  // Terminate early if the absolute improvement is small.
    double_t min_relative_per_step_improvement = 1e-25;  // Terminate early if the relative improvement is small.
    double_t no_improvement_steps_to_terminate = 2;      // Wait for this # of consecutive no improvement iterations.

    if (Exists(super.Parameters())) {
      const auto& parameters = Value(super.Parameters());
      min_steps = parameters.GetValue("min_steps", min_steps);
      max_steps = parameters.GetValue("max_steps", max_steps);
      bt_alpha = parameters.GetValue("bt_alpha", bt_alpha);
      bt_beta = parameters.GetValue("bt_beta", bt_beta);
      bt_max_steps = parameters.GetValue("bt_max_steps", bt_max_steps);
      grad_eps = parameters.GetValue("grad_eps", grad_eps);
      min_relative_per_step_improvement =
          parameters.GetValue("min_relative_per_step_improvement", min_relative_per_step_improvement);
      min_absolute_per_step_improvement =
          parameters.GetValue("min_absolute_per_step_improvement", min_absolute_per_step_improvement);
      no_improvement_steps_to_terminate =
          parameters.GetValue("no_improvement_steps_to_terminate", no_improvement_steps_to_terminate);
    }

    logger.Log("GradientDescentOptimizerBT: Begin at " + super.PointAsString(starting_point));

    size_t iteration;
    int no_improvement_steps = 0;

    ValueAndPoint current(f(starting_point), starting_point);

    {
      OptimizerStats stats("GradientDescentOptimizerBT");
      for (iteration = 0; iteration < max_steps; ++iteration) {
        if (super.StoppingCriterionSatisfied(iteration, current.point) == EarlyStoppingCriterion::StopOptimization) {
          logger.Log("GradientDescentOptimizer: External stopping criterion satisfied, terminating.");
          break;
        }

        stats.JournalIteration();
        if (logger) {
          // Expensive call, don't make it if `logger` is not initialized.
          logger.Log("GradientDescentOptimizerBT: Iteration " + current::ToString(iteration + 1) + ", OF = " +
                     current::ToString(current.value) + " @ " + super.PointAsString(current.point));
        }
        auto direction = g(current.point);
        // Simple early stopping by the norm of the gradient.
        if (std::sqrt(fncas::L2Norm(direction)) < grad_eps && iteration >= min_steps) {
          logger.Log("GradientDescentOptimizerBT: Terminating due to small gradient norm.");
          break;
        }

        fncas::FlipSign(direction);  // Going against the gradient to minimize the function.

        try {
          const auto next = Backtracking(f, g, current.point, direction, stats, bt_alpha, bt_beta, bt_max_steps);

          if (!IsNormal(next.value)) {
            // Would never happen as `BacktrackingException` is caught below, but just to be safe.
            CURRENT_THROW(FnCASOptimizationException("!fncas::IsNormal(next.value)"));
          }

          if (next.value / current.value > 1.0 - min_relative_per_step_improvement ||
              current.value - next.value < min_absolute_per_step_improvement) {
            ++no_improvement_steps;
            if (no_improvement_steps >= no_improvement_steps_to_terminate) {
              logger.Log("GradientDescentOptimizerBT: Terminating due to no improvement.");
              break;
            }
          } else {
            no_improvement_steps = 0;
          }

          current = next;
        } catch (const BacktrackingException&) {
          logger.Log("GradientDescentOptimizerBT: Terminating due to no backtracking step possible.");
          break;
        }
      }
    }

    logger.Log("GradientDescentOptimizerBT: Result = " + super.PointAsString(current.point));
    logger.Log("GradientDescentOptimizerBT: Objective function = " + current::ToString(current.value));

    return current;
  }
};

// Optimizer that uses a combination of conjugate gradient method and
// backtracking line search to find a local minimum of `F::ObjectiveFunction` function.
struct ConjugateGradientOptimizerSelector;

template <class F>
class ConjugateGradientOptimizer final : public OptimizeInvoker<F, ConjugateGradientOptimizerSelector> {
 public:
  using super_t = OptimizeInvoker<F, ConjugateGradientOptimizerSelector>;
  using super_t::super_t;
};

template <>
struct OptimizeImpl<ConjugateGradientOptimizerSelector> {
  template <typename ORIGINAL_F, typename F, typename G>
  static OptimizationResult RunOptimize(const Optimizer<ORIGINAL_F>& super,
                                        F&& f,
                                        G&& g,
                                        const std::vector<double_t>& starting_point) {
    // TODO(mzhurovich): Implement a more sophisticated version.
    const auto& logger = OptimizerLogger();

    size_t min_steps = 3;       // Minimum number of optimization steps (ignoring early stopping).
    size_t max_steps = 250;     // Maximum number of optimization steps.
    double_t bt_alpha = 0.5;    // Alpha parameter for backtracking algorithm.
    double_t bt_beta = 0.8;     // Beta parameter for backtracking algorithm.
    size_t bt_max_steps = 100;  // Maximum number of backtracking steps.
    double_t grad_eps = 1e-8;   // Magnitude of gradient for early stopping.
    double_t min_absolute_per_step_improvement = 1e-25;  // Terminate early if the absolute improvement is small.
    double_t min_relative_per_step_improvement = 1e-25;  // Terminate early if the relative improvement is small.
    double_t no_improvement_steps_to_terminate = 2;      // Wait for this # of consecutive no improvement iterations.

    if (Exists(super.Parameters())) {
      const auto& parameters = Value(super.Parameters());
      min_steps = parameters.GetValue("min_steps", min_steps);
      max_steps = parameters.GetValue("max_steps", max_steps);
      bt_alpha = parameters.GetValue("bt_alpha", bt_alpha);
      bt_beta = parameters.GetValue("bt_beta", bt_beta);
      bt_max_steps = parameters.GetValue("bt_max_steps", bt_max_steps);
      grad_eps = parameters.GetValue("grad_eps", grad_eps);
      min_relative_per_step_improvement =
          parameters.GetValue("min_relative_per_step_improvement", min_relative_per_step_improvement);
      min_absolute_per_step_improvement =
          parameters.GetValue("min_absolute_per_step_improvement", min_absolute_per_step_improvement);
      no_improvement_steps_to_terminate =
          parameters.GetValue("no_improvement_steps_to_terminate", no_improvement_steps_to_terminate);
    }

    logger.Log("ConjugateGradientOptimizer: The objective function with its gradient is " +
               current::ToString(node_vector_singleton().size()) + " nodes.");

    ValueAndPoint current(f(starting_point), starting_point);
    logger.Log("ConjugateGradientOptimizer: Original objective function = " + current::ToString(current.value));
    if (!fncas::IsNormal(current.value)) {
      CURRENT_THROW(FnCASOptimizationException("!fncas::IsNormal(current.value)"));
    }

    std::vector<double_t> current_gradient = g(current.point);
    std::vector<double_t> s(current_gradient);  // Direction to search for a minimum.
    fncas::FlipSign(s);                         // Trying first step against the gradient to minimize the function.

    logger.Log("ConjugateGradientOptimizer: Begin at " + super.PointAsString(starting_point));
    size_t iteration;
    int no_improvement_steps = 0;
    {
      OptimizerStats stats("ConjugateGradientOptimizer");
      for (iteration = 0; iteration < max_steps; ++iteration) {
        if (super.StoppingCriterionSatisfied(iteration, current.point) == EarlyStoppingCriterion::StopOptimization) {
          logger.Log("GradientDescentOptimizer: External stopping criterion satisfied, terminating.");
          break;
        }

        stats.JournalIteration();
        if (logger) {
          // Expensive call, don't make it if `logger` is not initialized.
          logger.Log("ConjugateGradientOptimizer: Iteration " + current::ToString(iteration + 1) + ", OF = " +
                     current::ToString(current.value) + " @ " + super.PointAsString(current.point));
        }
        try {
          // Backtracking line search.
          const auto next = Backtracking(f, g, current.point, s, stats, bt_alpha, bt_beta, bt_max_steps);

          if (!IsNormal(next.value)) {
            // Would never happen as `BacktrackingException` is caught below, but just to be safe.
            CURRENT_THROW(FnCASOptimizationException("!fncas::IsNormal(next.value)"));
          }

          stats.JournalGradient();
          const auto new_gradient = g(next.point);

          // Calculating direction for the next step.
          const double_t omega =
              std::max(fncas::PolakRibiere(new_gradient, current_gradient), static_cast<double_t>(0));
          s = SumVectors(s, new_gradient, omega, -1.0);

          if (next.value / current.value > 1.0 - min_relative_per_step_improvement ||
              current.value - next.value < min_absolute_per_step_improvement) {
            ++no_improvement_steps;
            if (no_improvement_steps >= no_improvement_steps_to_terminate) {
              logger.Log("ConjugateGradientOptimizer: Terminating due to no improvement.");
              break;
            }
          } else {
            no_improvement_steps = 0;
          }

          current = next;
          current_gradient = new_gradient;

          // Simple early stopping by the norm of the gradient.
          if (std::sqrt(L2Norm(s)) < grad_eps && iteration >= min_steps) {
            break;
          }
        } catch (const BacktrackingException&) {
          logger.Log("GradientDescentOptimizerBT: Terminating due to no backtracking step possible.");
          break;
        }
      }
    }

    logger.Log("ConjugateGradientOptimizer: Result = " + super.PointAsString(current.point));
    logger.Log("ConjugateGradientOptimizer: Objective function = " + current::ToString(current.value));

    return current;
  }
};

}  // namespace fncas

#endif  // #ifndef FNCAS_OPTIMIZE_H
