/*
Copyright (c) 2015-2020, Intel Corporation
Copyright (c) 2019-2020, University of Utah

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

     http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
*/

//////////////////////////////////////////////////////////////////////////////

#include "data.h"
#include "context.h"
#include "expr.h"

#include <utility>

using namespace yarpgen;

void ScalarVar::dbgDump() {
    std::cout << "Scalar var: " << name << std::endl;
    std::cout << "Type info:" << std::endl;
    type->dbgDump();
    std::cout << "Init val: " << init_val << std::endl;
    std::cout << "Current val: " << cur_val << std::endl;
    std::cout << "Was changed: " << changed << std::endl;
}

std::shared_ptr<ScalarVar> ScalarVar::create(std::shared_ptr<PopulateCtx> ctx) {
    auto gen_pol = ctx->getGenPolicy();
    IntTypeID type_id = rand_val_gen->getRandId(gen_pol->int_type_distr);
    IRValue init_val = rand_val_gen->getRandValue(type_id);
    auto int_type = IntegralType::init(type_id);
    NameHandler &nh = NameHandler::getInstance();
    return std::make_shared<ScalarVar>(nh.getVarName(), int_type, init_val);
}

std::string ScalarVar::getName(std::shared_ptr<EmitCtx> ctx) {
    if (!ctx)
        ERROR("Can't give a name without a context");

    std::string ret;
    if (!ctx->getSYCLPrefix().empty())
        ret = ctx->getSYCLPrefix();
    ret += Data::getName(ctx);
    if (ctx->useSYCLAccess())
        ret += "[0]";
    return ret;
}

void Array::dbgDump() {
    std::cout << "Array: " << name << std::endl;
    std::cout << "Type info:" << std::endl;
    type->dbgDump();
    init_vals->dbgDump();
    cur_vals->dbgDump();
}

Array::Array(std::string _name, const std::shared_ptr<ArrayType> &_type,
             std::shared_ptr<Data> _val)
    : Data(std::move(_name), _type), init_vals(_val), cur_vals(_val),
      was_changed(false) {
    if (!type->isArrayType())
        ERROR("Array variable should have an ArrayType");

    auto array_type = std::static_pointer_cast<ArrayType>(type);
    if (array_type->getBaseType() != init_vals->getType())
        ERROR("Can't initialize array with variable of wrong type");

    ub_code = init_vals->getUBCode();
}

void Array::setValue(std::shared_ptr<Data> _val) {
    /*
    auto array_type = std::static_pointer_cast<ArrayType>(type);
    if (array_type->getBaseType() != _val->getType())
        ERROR("Can't initialize array with variable of wrong type");
    */
    cur_vals = _val;
    ub_code = cur_vals->getUBCode();
    was_changed = true;
}

std::shared_ptr<Array> Array::create(std::shared_ptr<PopulateCtx> ctx,
                                     bool inp) {
    std::vector<std::shared_ptr<Iterator>> used_iters;
    auto array_type = ArrayType::create(ctx, used_iters);
    auto base_type = array_type->getBaseType();
    if (!base_type->isIntType())
        ERROR("We support only array of integers for now");
    auto int_type = std::static_pointer_cast<IntegralType>(base_type);
    IRValue init_val = rand_val_gen->getRandValue(int_type->getIntTypeId());
    auto init_var = std::make_shared<ScalarVar>("", int_type, init_val);
    NameHandler &nh = NameHandler::getInstance();
    auto new_array =
        std::make_shared<Array>(nh.getArrayName(), array_type, init_var);
    std::shared_ptr<Expr> prev_expr = std::make_shared<ArrayUseExpr>(new_array);
    for (auto &used_iter : used_iters) {
        auto iter_use_expr = std::make_shared<IterUseExpr>(used_iter);
        prev_expr = std::make_shared<SubscriptExpr>(prev_expr, iter_use_expr);
    }
    if (inp)
        ctx->getLocalSymTable()->addSubsExpr(
            std::static_pointer_cast<SubscriptExpr>(prev_expr));
    else
        ctx->getExtOutSymTable()->addSubsExpr(
            std::static_pointer_cast<SubscriptExpr>(prev_expr));
    return new_array;
}

void Iterator::setParameters(std::shared_ptr<Expr> _start,
                             std::shared_ptr<Expr> _end,
                             std::shared_ptr<Expr> _step) {
    // TODO: if start equals to end, then the iterator should be degenerate
    start = std::move(_start);
    end = std::move(_end);
    step = std::move(_step);
}

std::shared_ptr<Iterator> Iterator::create(std::shared_ptr<GenCtx> ctx,
                                           bool is_uniform) {
    // TODO: this function is full of magic constants and weird hacks to cut
    //  some corners for ISPC and overflows
    auto gen_pol = ctx->getGenPolicy();

    IntTypeID type_id = rand_val_gen->getRandId(gen_pol->int_type_distr);
    std::shared_ptr<Type> type = IntegralType::init(type_id);
    if (!is_uniform)
        type = type->makeVarying();
    auto int_type = std::static_pointer_cast<IntegralType>(type);

    auto start = std::make_shared<ConstantExpr>(IRValue{type_id, {false, 0}});

    size_t end_val = rand_val_gen->getRandValue(gen_pol->iters_end_limit_min,
                                                gen_pol->iter_end_limit_max);
    // We can't go pass the maximal value of the type
    end_val =
        std::min((uint64_t)end_val, int_type->getMax().getAbsValue().value);
    // TODO: ISPC doesn't execute division under mask, so the easiest way to
    // eliminate UB problems is to make sure that iterator doesn't go outside
    // array boundaries
    if (!is_uniform)
        end_val = (end_val / 64) * 64;
    auto end =
        std::make_shared<ConstantExpr>(IRValue(type_id, {false, end_val}));

    size_t step_val = rand_val_gen->getRandId(gen_pol->iters_step_distr);
    if (!is_uniform)
        step_val = 1;
    // We can't overflow uncontrollably
    // TODO: we need to support controlled overflow and better strategy
    if ((end_val / step_val + 1) * step_val >=
        int_type->getMax().getAbsValue().value)
        step_val = 1;
    auto step =
        std::make_shared<ConstantExpr>(IRValue{type_id, {false, step_val}});

    NameHandler &nh = NameHandler::getInstance();
    auto iter = std::make_shared<Iterator>(nh.getIterName(), type, start, end,
                                           step, end_val == 0);

    return iter;
}

void Iterator::dbgDump() {
    std::cout << name << std::endl;
    type->dbgDump();
    auto emit_ctx = std::make_shared<EmitCtx>();
    start->emit(emit_ctx, std::cout);
    end->emit(emit_ctx, std::cout);
    end->emit(emit_ctx, std::cout);
}

// This function bring the value of an expression that used in iterator
// specification to the desired value, using additions
static std::shared_ptr<Expr> adjustIterExprValue(std::shared_ptr<Expr> expr,
                                                 IRValue value) {
    EvalCtx eval_ctx;
    auto eval_res = expr->evaluate(eval_ctx);

    if (!eval_res->getType()->isIntType())
        ERROR("We support only integer types for now");

    auto int_type = std::static_pointer_cast<IntegralType>(eval_res->getType());
    if (int_type->getIntTypeId() != value.getIntTypeID())
        ERROR("You are supposed to handle cast first");

    if (eval_res->getKind() != DataKind::VAR)
        ERROR("We support only scalar variables for now");

    // ISPC cast to uniform. Despite the fact that iterator can have varying
    // type, its initialization expression should always have uniform type
    Options &options = Options::getInstance();
    if (options.isISPC())
        if (!eval_res->getType()->isUniform())
            expr = std::make_shared<ExtractCall>(expr);

    // Every binary operation applies integral promotion first, so we need to
    // guarantee that expression can be processed
    if (int_type->getIntTypeId() < IntTypeID::INT) {
        expr = std::make_shared<TypeCastExpr>(
            expr, IntegralType::init(IntTypeID::INT), true);
        value = value.castToType(IntTypeID::INT);
    }

    std::shared_ptr<Expr> ret = expr;
    IRValue expr_val;
    IRValue zero = value;
    zero.setValue(IRValue::AbsValue{false, 0});
    // TODO: is it enough to perform these checks only once?
    do {
        eval_res = ret->evaluate(eval_ctx);
        expr_val =
            std::static_pointer_cast<ScalarVar>(eval_res)->getCurrentValue();

        IRValue diff;
        if ((value > expr_val).getValueRef<bool>())
            diff = value - expr_val;
        else
            diff = expr_val - value;

        if (diff.hasUB())
            diff = int_type->getMax();

        if ((diff == zero).getValueRef<bool>())
            break;

        if ((value > expr_val).getValueRef<bool>())
            ret = std::make_shared<BinaryExpr>(
                BinaryOp::ADD, ret, std::make_shared<ConstantExpr>(diff));
        else
            ret = std::make_shared<BinaryExpr>(
                BinaryOp::SUB, ret, std::make_shared<ConstantExpr>(diff));
    } while (true);

    return ret;
}

void Iterator::populate(std::shared_ptr<PopulateCtx> ctx) {
    auto gen_pol = ctx->getGenPolicy();

    Options &options = Options::getInstance();

    auto populate_impl =
        [&ctx, &gen_pol,
         &options](std::shared_ptr<Type> type,
                   std::shared_ptr<Expr> expr) -> std::shared_ptr<Expr> {
        LoopEndKind loop_end_kind =
            rand_val_gen->getRandId(gen_pol->loop_end_kind_distr);
        // TODO: add fall-back safety mechanism
        std::shared_ptr<Expr> ret = expr;
        if (loop_end_kind == LoopEndKind::CONST)
            return ret;
        else if (loop_end_kind == LoopEndKind::VAR) {
            // TODO: add appropriate option
            DataKind data_kind =
                rand_val_gen->getRandId(gen_pol->out_kind_distr);
            if (data_kind == DataKind::VAR || type->isUniform() ||
                (data_kind == DataKind::ARR &&
                 ctx->getExtInpSymTable()->getAvailSubs().empty())) {
                auto new_scalar_var_expr = ScalarVarUseExpr::create(ctx);
                new_scalar_var_expr->setIsDead(false);
                ret = new_scalar_var_expr;
            }
            else if (data_kind == DataKind::ARR) {
                auto new_subs_expr = SubscriptExpr::create(ctx);
                new_subs_expr->setIsDead(false);
                ret = new_subs_expr;
            }
            else
                ERROR("Bad data kind");
        }
        else if (loop_end_kind == LoopEndKind::EXPR) {
            ret = ArithmeticExpr::create(ctx);
        }
        else
            ERROR("Bad Loop End Kind");

        EvalCtx eval_ctx;
        auto ret_eval_res = ret->rebuild(eval_ctx);

        if (!type->isIntType() || !ret_eval_res->getType()->isIntType())
            ERROR("We support only integer types for now");

        auto int_type = std::static_pointer_cast<IntegralType>(type);
        auto int_eval_res_type =
            std::static_pointer_cast<IntegralType>(ret_eval_res->getType());

        if (options.isISPC())
            if (!ret_eval_res->getType()->isUniform()) {
                ret = std::make_shared<ExtractCall>(ret);
                ret_eval_res = ret->rebuild(eval_ctx);
                int_eval_res_type = std::static_pointer_cast<IntegralType>(
                    ret_eval_res->getType());
            }

        if (int_type->getIntTypeId() != int_eval_res_type->getIntTypeId()) {
            ret = std::make_shared<TypeCastExpr>(
                ret, IntegralType::init(int_type->getIntTypeId()), true);
            ret_eval_res = ret->rebuild(eval_ctx);
        }

        auto expr_eval_res = expr->evaluate(eval_ctx);
        if (expr_eval_res->getKind() != DataKind::VAR)
            ERROR("We support only integer variables for now");

        ret = adjustIterExprValue(
            ret, std::static_pointer_cast<ScalarVar>(expr_eval_res)
                     ->getCurrentValue());

        return ret;
    };

    start = populate_impl(type, start);
    end = populate_impl(type, end);
    step = populate_impl(type, step);
}
