/*
 * Copyright © 2010 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

/**
 * \file ir_mat_op_to_vec.cpp
 *
 * Breaks matrix operation expressions down to a series of vector operations.
 *
 * Generally this is how we have to codegen matrix operations for a
 * GPU, so this gives us the chance to constant fold operations on a
 * column or row.
 */

#include "ir.h"
#include "ir_expression_flattening.h"
#include "glsl_types.h"

class ir_mat_op_to_vec_visitor : public ir_hierarchical_visitor {
public:
   ir_mat_op_to_vec_visitor()
   {
      this->made_progress = false;
   }

   ir_visitor_status visit_leave(ir_assignment *);

   ir_dereference *get_column(ir_variable *var, int col);
   ir_rvalue *get_element(ir_variable *var, int col, int row);

   void do_mul_mat_mat(ir_variable *result_var,
		       ir_variable *a_var, ir_variable *b_var);
   void do_mul_mat_vec(ir_variable *result_var,
		       ir_variable *a_var, ir_variable *b_var);
   void do_mul_vec_mat(ir_variable *result_var,
		       ir_variable *a_var, ir_variable *b_var);
   void do_mul_mat_scalar(ir_variable *result_var,
			  ir_variable *a_var, ir_variable *b_var);

   bool made_progress;
};

static bool
mat_op_to_vec_predicate(ir_instruction *ir)
{
   ir_expression *expr = ir->as_expression();
   unsigned int i;

   if (!expr)
      return false;

   for (i = 0; i < expr->get_num_operands(); i++) {
      if (expr->operands[i]->type->is_matrix())
	 return true;
   }

   return false;
}

bool
do_mat_op_to_vec(exec_list *instructions)
{
   ir_mat_op_to_vec_visitor v;

   /* Pull out any matrix expression to a separate assignment to a
    * temp.  This will make our handling of the breakdown to
    * operations on the matrix's vector components much easier.
    */
   do_expression_flattening(instructions, mat_op_to_vec_predicate);

   visit_list_elements(&v, instructions);

   return v.made_progress;
}

ir_rvalue *
ir_mat_op_to_vec_visitor::get_element(ir_variable *var, int col, int row)
{
   ir_dereference *deref;

   deref = new(base_ir) ir_dereference_variable(var);

   if (var->type->is_matrix()) {
      deref = new(base_ir) ir_dereference_array(var,
						new(base_ir) ir_constant(col));
   } else {
      assert(col == 0);
   }

   return new(base_ir) ir_swizzle(deref, row, 0, 0, 0, 1);
}

ir_dereference *
ir_mat_op_to_vec_visitor::get_column(ir_variable *var, int row)
{
   ir_dereference *deref;

   if (!var->type->is_matrix()) {
      deref = new(base_ir) ir_dereference_variable(var);
   } else {
      deref = new(base_ir) ir_dereference_variable(var);
      deref = new(base_ir) ir_dereference_array(deref,
						new(base_ir) ir_constant(row));
   }

   return deref;
}

void
ir_mat_op_to_vec_visitor::do_mul_mat_mat(ir_variable *result_var,
					 ir_variable *a_var,
					 ir_variable *b_var)
{
   int b_col, i;
   ir_assignment *assign;
   ir_expression *expr;

   for (b_col = 0; b_col < b_var->type->matrix_columns; b_col++) {
      ir_rvalue *a = get_column(a_var, 0);
      ir_rvalue *b = get_element(b_var, b_col, 0);

      /* first column */
      expr = new(base_ir) ir_expression(ir_binop_mul,
					a->type,
					a,
					b);

      /* following columns */
      for (i = 1; i < a_var->type->matrix_columns; i++) {
	 ir_expression *mul_expr;

	 a = get_column(a_var, i);
	 b = get_element(b_var, b_col, i);

	 mul_expr = new(base_ir) ir_expression(ir_binop_mul,
					       a->type,
					       a,
					       b);
	 expr = new(base_ir) ir_expression(ir_binop_add,
					   a->type,
					   expr,
					   mul_expr);
      }

      ir_rvalue *result = get_column(result_var, b_col);
      assign = new(base_ir) ir_assignment(result,
					  expr,
					  NULL);
      base_ir->insert_before(assign);
   }
}

void
ir_mat_op_to_vec_visitor::do_mul_mat_vec(ir_variable *result_var,
					 ir_variable *a_var,
					 ir_variable *b_var)
{
   int i;
   ir_rvalue *a = get_column(a_var, 0);
   ir_rvalue *b = get_element(b_var, 0, 0);
   ir_assignment *assign;
   ir_expression *expr;

   /* first column */
   expr = new(base_ir) ir_expression(ir_binop_mul,
				     result_var->type,
				     a,
				     b);

   /* following columns */
   for (i = 1; i < a_var->type->matrix_columns; i++) {
      ir_expression *mul_expr;

      a = get_column(a_var, i);
      b = get_element(b_var, 0, i);

      mul_expr = new(base_ir) ir_expression(ir_binop_mul,
					    result_var->type,
					    a,
					    b);
      expr = new(base_ir) ir_expression(ir_binop_add,
					result_var->type,
					expr,
					mul_expr);
   }

   ir_rvalue *result = new(base_ir) ir_dereference_variable(result_var);
   assign = new(base_ir) ir_assignment(result,
				       expr,
				       NULL);
   base_ir->insert_before(assign);
}

void
ir_mat_op_to_vec_visitor::do_mul_vec_mat(ir_variable *result_var,
					 ir_variable *a_var,
					 ir_variable *b_var)
{
   int i;

   for (i = 0; i < b_var->type->matrix_columns; i++) {
      ir_rvalue *a = new(base_ir) ir_dereference_variable(a_var);
      ir_rvalue *b = get_column(b_var, i);
      ir_rvalue *result;
      ir_expression *column_expr;
      ir_assignment *column_assign;

      result = new(base_ir) ir_dereference_variable(result_var);
      result = new(base_ir) ir_swizzle(result, i, 0, 0, 0, 1);

      column_expr = new(base_ir) ir_expression(ir_binop_dot,
					       result->type,
					       a,
					       b);

      column_assign = new(base_ir) ir_assignment(result,
						 column_expr,
						 NULL);
      base_ir->insert_before(column_assign);
   }
}

void
ir_mat_op_to_vec_visitor::do_mul_mat_scalar(ir_variable *result_var,
					    ir_variable *a_var,
					    ir_variable *b_var)
{
   int i;

   for (i = 0; i < a_var->type->matrix_columns; i++) {
      ir_rvalue *a = get_column(a_var, i);
      ir_rvalue *b = new(base_ir) ir_dereference_variable(b_var);
      ir_rvalue *result = get_column(result_var, i);
      ir_expression *column_expr;
      ir_assignment *column_assign;

      column_expr = new(base_ir) ir_expression(ir_binop_mul,
					       result->type,
					       a,
					       b);

      column_assign = new(base_ir) ir_assignment(result,
						 column_expr,
						 NULL);
      base_ir->insert_before(column_assign);
   }
}

ir_visitor_status
ir_mat_op_to_vec_visitor::visit_leave(ir_assignment *assign)
{
   ir_expression *expr = assign->rhs->as_expression();
   bool found_matrix = false;
   unsigned int i, matrix_columns = 1;
   ir_variable *op_var[2];

   if (!expr)
      return visit_continue;

   for (i = 0; i < expr->get_num_operands(); i++) {
      if (expr->operands[i]->type->is_matrix()) {
	 found_matrix = true;
	 matrix_columns = expr->operands[i]->type->matrix_columns;
	 break;
      }
   }
   if (!found_matrix)
      return visit_continue;

   ir_dereference_variable *lhs_deref = assign->lhs->as_dereference_variable();
   assert(lhs_deref);

   ir_variable *result_var = lhs_deref->var;

   /* Store the expression operands in temps so we can use them
    * multiple times.
    */
   for (i = 0; i < expr->get_num_operands(); i++) {
      ir_assignment *assign;

      op_var[i] = new(base_ir) ir_variable(expr->operands[i]->type,
					   "mat_op_to_vec",
					   ir_var_temporary);
      base_ir->insert_before(op_var[i]);

      lhs_deref = new(base_ir) ir_dereference_variable(op_var[i]);
      assign = new(base_ir) ir_assignment(lhs_deref,
					  expr->operands[i],
					  NULL);
      base_ir->insert_before(assign);
   }

   /* OK, time to break down this matrix operation. */
   switch (expr->operation) {
   case ir_unop_neg: {
      const unsigned mask = (1U << result_var->type->vector_elements) - 1;

      /* Apply the operation to each column.*/
      for (i = 0; i < matrix_columns; i++) {
	 ir_rvalue *op0 = get_column(op_var[0], i);
	 ir_dereference *result = get_column(result_var, i);
	 ir_expression *column_expr;
	 ir_assignment *column_assign;

	 column_expr = new(base_ir) ir_expression(expr->operation,
						  result->type,
						  op0,
						  NULL);

	 column_assign = new(base_ir) ir_assignment(result,
						    column_expr,
						    NULL,
						    mask);
	 assert(column_assign->write_mask != 0);
	 base_ir->insert_before(column_assign);
      }
      break;
   }
   case ir_binop_add:
   case ir_binop_sub:
   case ir_binop_div:
   case ir_binop_mod: {
      const unsigned mask = (1U << result_var->type->vector_elements) - 1;

      /* For most operations, the matrix version is just going
       * column-wise through and applying the operation to each column
       * if available.
       */
      for (i = 0; i < matrix_columns; i++) {
	 ir_rvalue *op0 = get_column(op_var[0], i);
	 ir_rvalue *op1 = get_column(op_var[1], i);
	 ir_dereference *result = get_column(result_var, i);
	 ir_expression *column_expr;
	 ir_assignment *column_assign;

	 column_expr = new(base_ir) ir_expression(expr->operation,
						  result->type,
						  op0,
						  op1);

	 column_assign = new(base_ir) ir_assignment(result,
						    column_expr,
						    NULL,
						    mask);
	 assert(column_assign->write_mask != 0);
	 base_ir->insert_before(column_assign);
      }
      break;
   }
   case ir_binop_mul:
      if (op_var[0]->type->is_matrix()) {
	 if (op_var[1]->type->is_matrix()) {
	    do_mul_mat_mat(result_var, op_var[0], op_var[1]);
	 } else if (op_var[1]->type->is_vector()) {
	    do_mul_mat_vec(result_var, op_var[0], op_var[1]);
	 } else {
	    assert(op_var[1]->type->is_scalar());
	    do_mul_mat_scalar(result_var, op_var[0], op_var[1]);
	 }
      } else {
	 assert(op_var[1]->type->is_matrix());
	 if (op_var[0]->type->is_vector()) {
	    do_mul_vec_mat(result_var, op_var[0], op_var[1]);
	 } else {
	    assert(op_var[0]->type->is_scalar());
	    do_mul_mat_scalar(result_var, op_var[1], op_var[0]);
	 }
      }
      break;
   default:
      printf("FINISHME: Handle matrix operation for %s\n", expr->operator_string());
      abort();
   }
   assign->remove();
   this->made_progress = true;

   return visit_continue;
}
