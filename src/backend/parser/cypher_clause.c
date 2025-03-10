/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

#include "postgres.h"

#include "access/sysattr.h"
#include "catalog/pg_type_d.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "nodes/nodes.h"
#include "nodes/parsenodes.h"
#include "nodes/pg_list.h"
#include "nodes/primnodes.h"
#include "optimizer/var.h"
#include "parser/parse_clause.h"
#include "parser/parse_coerce.h"
#include "parser/parse_collate.h"
#include "parser/parse_expr.h"
#include "parser/parse_func.h"
#include "parser/parse_node.h"
#include "parser/parse_oper.h"
#include "parser/parse_relation.h"
#include "parser/parse_target.h"
#include "parser/parsetree.h"
#include "rewrite/rewriteHandler.h"
#include "utils/typcache.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"

#include "catalog/ag_graph.h"
#include "catalog/ag_label.h"
#include "commands/label_commands.h"
#include "nodes/ag_nodes.h"
#include "nodes/cypher_nodes.h"
#include "parser/cypher_clause.h"
#include "parser/cypher_expr.h"
#include "parser/cypher_item.h"
#include "parser/cypher_parse_agg.h"
#include "parser/cypher_parse_node.h"
#include "utils/ag_cache.h"
#include "utils/ag_func.h"
#include "utils/agtype.h"
#include "utils/graphid.h"

/*
 * Variable string names for makeTargetEntry. As they are going to be variable
 * names that will be hidden from the user, we need to do our best to make sure
 * they won't be picked by mistake. Additionally, their form needs to be easily
 * determined as ours. For now, prefix them as follows -
 *
 *     #define AGE_VARNAME_SOMETHING AGE_DEFAULT_VARNAME_PREFIX"something"
 *
 * We should probably make an automated variable generator, like for aliases,
 * for this.
 *
 * Also, keep these here as nothing outside of this file needs to know these.
 */
#define AGE_VARNAME_CREATE_CLAUSE AGE_DEFAULT_VARNAME_PREFIX"create_clause"
#define AGE_VARNAME_CREATE_NULL_VALUE AGE_DEFAULT_VARNAME_PREFIX"create_null_value"
#define AGE_VARNAME_DELETE_CLAUSE AGE_DEFAULT_VARNAME_PREFIX"delete_clause"
#define AGE_VARNAME_ID AGE_DEFAULT_VARNAME_PREFIX"id"
#define AGE_VARNAME_SET_CLAUSE AGE_DEFAULT_VARNAME_PREFIX"set_clause"

enum transform_entity_type
{
    ENT_VERTEX = 0x0,
    ENT_EDGE
};

enum transform_entity_join_side
{
    JOIN_SIDE_LEFT = 0x0,
    JOIN_SIDE_RIGHT
};

/*
 * In the transformation stage, we need to track
 * where a variable came from. When moving between
 * clauses, Postgres parsestate and Query data structures
 * are insufficient for some of the information we
 * need.
 */
typedef struct
{
    // denotes whether this entity is a vertex or edge
    enum transform_entity_type type;

    /*
     * MATCH clauses are transformed into a select * FROM ... JOIN, etc
     * We need to know wheter the table that this entity represents is
     * part of the join tree. If a cypher_node does not meet the conditions
     * set in INCLUDE_NODE_IN_JOIN_TREE. Then we can skip the node when
     * constructing our join tree. The entities around this particular entity
     * need to know this for the join to get properly constructed.
     */
    bool in_join_tree;

    /*
     * The parse data structure will be transformed into an Expr that represents
     * the entity. When contructing the join tree, we need to know what it was
     * turned into. If the entity was originally created in a previous clause,
     * this will be a Var that we need to reference to extract the id, startid,
     * endid for the join. If the entity was created in the current clause, then
     * this will be a FuncExpr that we can reference to get the id, startid, and
     * endid.
     */
    Expr *expr;

    /*
     * tells each clause whether this variable was
     * declared by itself or a previous clause.
     */
    bool declared_in_current_clause;

    // The parse data structure that we transformed
    union
    {
        cypher_node *node;
        cypher_relationship *rel;
    } entity;
} transform_entity;

/*
 * Rules to determine if a node must be included:
 *
 *      1. the node is in a path variable
 *      2. the node is a variable
 *      3. the node contains filter properties
 */
#define INCLUDE_NODE_IN_JOIN_TREE(path, node) \
    (path->var_name || node->name || node->props)

typedef Query *(*transform_method)(cypher_parsestate *cpstate,
                                   cypher_clause *clause);

// projection
static Query *transform_cypher_return(cypher_parsestate *cpstate,
                                      cypher_clause *clause);
static List *transform_cypher_order_by(cypher_parsestate *cpstate,
                                       List *sort_items, List **target_list,
                                       ParseExprKind expr_kind);
static TargetEntry *find_target_list_entry(cypher_parsestate *cpstate,
                                           Node *node, List **target_list,
                                           ParseExprKind expr_kind);
static Node *transform_cypher_limit(cypher_parsestate *cpstate, Node *node,
                                    ParseExprKind expr_kind,
                                    const char *construct_name);
static Query *transform_cypher_with(cypher_parsestate *cpstate,
                                    cypher_clause *clause);
static Query *transform_cypher_clause_with_where(cypher_parsestate *cpstate,
                                                 transform_method transform,
                                                 cypher_clause *clause,
                                                 Node *where);
// match clause
static Query *transform_cypher_match(cypher_parsestate *cpstate,
                                     cypher_clause *clause);
static Query *transform_cypher_match_pattern(cypher_parsestate *cpstate,
                                             cypher_clause *clause);
static List *transform_match_entities(cypher_parsestate *cpstate, Query *query,
                                      cypher_path *path);
static void transform_match_pattern(cypher_parsestate *cpstate, Query *query,
                                    List *pattern);
static List *transform_match_path(cypher_parsestate *cpstate, Query *query,
                                  cypher_path *path);
static Expr *transform_cypher_edge(cypher_parsestate *cpstate,
                                   cypher_relationship *rel,
                                   List **target_list);
static Expr *transform_cypher_node(cypher_parsestate *cpstate,
                                   cypher_node *node, List **target_list,
                                   bool output_node);
static Node *make_vertex_expr(cypher_parsestate *cpstate, RangeTblEntry *rte,
                              char *label);
static Node *make_edge_expr(cypher_parsestate *cpstate, RangeTblEntry *rte,
                            char *label);
static FuncCall *make_qual(cypher_parsestate *cpstate,
                           transform_entity *entity, char *name);
static TargetEntry *
transform_match_create_path_variable(cypher_parsestate *cpstate,
                                     cypher_path *path, List *entities);
static List *make_path_join_quals(cypher_parsestate *cpstate, List *entities);
static List *make_directed_edge_join_conditions(
    cypher_parsestate *cpstate, transform_entity *prev_entity,
    transform_entity *next_entity, FuncCall *prev_qual, FuncCall *next_qual,
    char *prev_node_label, char *next_node_label);
static List *join_to_entity(cypher_parsestate *cpstate,
                            transform_entity *entity, FuncCall *qual,
                            enum transform_entity_join_side side);
static List *make_join_condition_for_edge(cypher_parsestate *cpstate,
                                          transform_entity *prev_edge,
                                          transform_entity *prev_node,
                                          transform_entity *entity,
                                          transform_entity *next_node,
                                          transform_entity *next_edge);
static List *make_edge_quals(cypher_parsestate *cpstate,
                             transform_entity *edge,
                             enum transform_entity_join_side side);
static A_Expr *filter_vertices_on_label_id(cypher_parsestate *cpstate,
                                           FuncCall *id_field, char *label);
static transform_entity *
make_transform_entity(cypher_parsestate *cpstate,
                      enum transform_entity_type type, Node *node, Expr *expr);
static transform_entity *find_variable(cypher_parsestate *cpstate, char *name);
static Node *create_property_constraint_function(cypher_parsestate *cpstate,
                                                 transform_entity *entity,
                                                 Node *property_constraints);
static TargetEntry *findTarget(List *targetList, char *resname);
// create clause
static Query *transform_cypher_create(cypher_parsestate *cpstate,
                                      cypher_clause *clause);
static List *transform_cypher_create_pattern(cypher_parsestate *cpstate,
                                             Query *query, List *pattern);
static cypher_create_path *
transform_cypher_create_path(cypher_parsestate *cpstate, List **target_list,
                             cypher_path *cp);
static cypher_target_node *
transform_create_cypher_node(cypher_parsestate *cpstate, List **target_list,
                             cypher_node *node);
static cypher_target_node *
transform_create_cypher_new_node(cypher_parsestate *cpstate,
                                 List **target_list, cypher_node *node);
static cypher_target_node *transform_create_cypher_existing_node(
    cypher_parsestate *cpstate, List **target_list, bool declared_in_current_clause,
    cypher_node *node);
static cypher_target_node *
transform_create_cypher_edge(cypher_parsestate *cpstate, List **target_list,
                             cypher_relationship *edge);
static Expr *cypher_create_properties(cypher_parsestate *cpstate,
                                      cypher_target_node *rel,
                                      Relation label_relation, Node *props,
                                      enum transform_entity_type type);
static Expr *add_volatile_wrapper(Expr *node);
static bool variable_exists(cypher_parsestate *cpstate, char *name);
static int get_target_entry_resno(List *target_list, char *name);
static TargetEntry *placeholder_target_entry(cypher_parsestate *cpstate,
                                             char *name);
static Query *transform_cypher_sub_pattern(cypher_parsestate *cpstate,
                                           cypher_clause *clause);
// set and remove clause
static Query *transform_cypher_set(cypher_parsestate *cpstate,
                                   cypher_clause *clause);
static cypher_update_information *transform_cypher_set_item_list(cypher_parsestate *cpstate,
                                                                 List *set_item_list,
                                                                 Query *query);
static cypher_update_information *transform_cypher_remove_item_list(cypher_parsestate *cpstate,
                                                                    List *remove_item_list,
                                                                    Query *query);
// delete
static Query *transform_cypher_delete(cypher_parsestate *cpstate,
                                      cypher_clause *clause);
static List *transform_cypher_delete_item_list(cypher_parsestate *cpstate,
                                               List *delete_item_list,
                                               Query *query);
// transform
#define PREV_CYPHER_CLAUSE_ALIAS "_"
#define transform_prev_cypher_clause(cpstate, prev_clause) \
    transform_cypher_clause_as_subquery(cpstate, transform_cypher_clause, \
                                        prev_clause)

static RangeTblEntry *transform_cypher_clause_as_subquery(cypher_parsestate *cpstate,
                                                          transform_method transform,
                                                          cypher_clause *clause);
static Query *analyze_cypher_clause(transform_method transform,
                                    cypher_clause *clause,
                                    cypher_parsestate *parent_cpstate);
static ParseNamespaceItem *makeNamespaceItem(RangeTblEntry *rte,
                                             bool rel_visible,
                                             bool cols_visible,
                                             bool lateral_only,
                                             bool lateral_ok);
static List *transform_group_clause(cypher_parsestate *cpstate,
                                    List *grouplist, List **groupingSets,
                                    List **targetlist, List *sortClause,
                                    ParseExprKind exprKind);
static Node *flatten_grouping_sets(Node *expr, bool toplevel,
                                   bool *hasGroupingSets);
static Index transform_group_clause_expr(List **flatresult,
                                         Bitmapset *seen_local,
                                         cypher_parsestate *cpstate,
                                         Node *gexpr, List **targetlist,
                                         List *sortClause,
                                         ParseExprKind exprKind,
                                         bool toplevel);
static List *add_target_to_group_list(cypher_parsestate *cpstate,
                                      TargetEntry *tle, List *grouplist,
                                      List *targetlist, int location);
static void advance_transform_entities_to_next_clause(List *entities);
/*
 * transform a cypher_clause
 */
Query *transform_cypher_clause(cypher_parsestate *cpstate,
                               cypher_clause *clause)
{
    Node *self = clause->self;
    Query *result;

    // examine the type of clause and call the transform logic for it
    if (is_ag_node(self, cypher_return))
        result = transform_cypher_return(cpstate, clause);
    else if (is_ag_node(self, cypher_with))
        return transform_cypher_with(cpstate, clause);
    else if (is_ag_node(self, cypher_match))
        return transform_cypher_match(cpstate, clause);
    else if (is_ag_node(self, cypher_create))
        result = transform_cypher_create(cpstate, clause);
    else if (is_ag_node(self, cypher_set))
        return transform_cypher_set(cpstate, clause);
    else if (is_ag_node(self, cypher_delete))
        return transform_cypher_delete(cpstate, clause);
    else if (is_ag_node(self, cypher_sub_pattern))
        result = transform_cypher_sub_pattern(cpstate, clause);
    else
        ereport(ERROR, (errmsg_internal("unexpected Node for cypher_clause")));

    result->querySource = QSRC_ORIGINAL;
    result->canSetTag = true;

    return result;
}

/*
 * Transform the Delete clause. Creates a _cypher_delete_clause
 * and passes the necessary information that is needed in the
 * execution phase.
 */
static Query *transform_cypher_delete(cypher_parsestate *cpstate,
                                      cypher_clause *clause)
{
    ParseState *pstate = (ParseState *)cpstate;
    cypher_delete *self = (cypher_delete *)clause->self;
    Query *query;
    TargetEntry *tle;
    Oid func_set_oid;
    Const *pattern_const;
    Expr *func_expr;
    RangeTblEntry *rte;
    int rtindex;
    StringInfo str = makeStringInfo();

    cypher_delete_information *delete_data;

    delete_data = make_ag_node(cypher_delete_information);

    query = makeNode(Query);
    query->commandType = CMD_SELECT;
    query->targetList = NIL;

    if (!clause->prev)
    {
        ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                        errmsg("DELETE cannot be the first clause in a Cypher query"),
                        parser_errposition(pstate, self->location)));
    }

    rte = transform_prev_cypher_clause(cpstate, clause->prev);
    rtindex = list_length(pstate->p_rtable);

    // rte is the first RangeTblEntry in pstate
    Assert(rtindex == 1);

    query->targetList = expandRelAttrs(pstate, rte, rtindex, 0, -1);

    func_set_oid = get_ag_func_oid(DELETE_CLAUSE_FUNCTION_NAME, 1, INTERNALOID);

    delete_data->delete_items = transform_cypher_delete_item_list(cpstate, self->exprs, query);
    delete_data->graph_name = cpstate->graph_name;
    delete_data->graph_oid = cpstate->graph_oid;
    delete_data->detach = self->detach;

    if (!clause->next)
        delete_data->flags |= CYPHER_CLAUSE_FLAG_TERMINAL;

    /*
     * Serialize the cypher_delete_information data structure. In certain
     * cases (Prepared Statements and PL/pgsql), the MemoryContext that
     * it is stored in will be destroyed. We need to get it into a format
     * that Postgres' can copy between MemoryContexts. Just making it into
     * an ExtensibleNode does not work, because there are certain parts of
     * Postgres that cannot handle an ExtensibleNode in a function call.
     * So we serialize the data structure and place it into a Const node
     * that can handle these situations AND be copied correctly.
     */
    outNode(str, delete_data);

    pattern_const = makeConst(INTERNALOID, -1, InvalidOid, str->len,
                              PointerGetDatum(str->data), false, false);

    func_expr = (Expr *)makeFuncExpr(func_set_oid, AGTYPEOID,
                                     list_make1(pattern_const), InvalidOid,
                                     InvalidOid, COERCE_EXPLICIT_CALL);

    // Create the target entry
    tle = makeTargetEntry(func_expr, pstate->p_next_resno++,
                          AGE_VARNAME_DELETE_CLAUSE, false);
    query->targetList = lappend(query->targetList, tle);

    query->rtable = pstate->p_rtable;
    query->jointree = makeFromExpr(pstate->p_joinlist, NULL);

    return query;
}

/*
 * Iterate through the list of items to delete and extract the variable name.
 * Then find the resno that the variable name belongs to.
 */
static List *transform_cypher_delete_item_list(cypher_parsestate *cpstate,
                                               List *delete_item_list, Query *query)
{
    ParseState *pstate = (ParseState *)cpstate;
    List *items = NIL;
    ListCell *lc;

    foreach(lc, delete_item_list)
    {
        Node *expr = lfirst(lc);
        ColumnRef *col;
        Value *val, *pos;
        int resno;

        cypher_delete_item *item = make_ag_node(cypher_delete_item);

        if (!IsA(expr, ColumnRef))
            ereport(ERROR, (errmsg_internal("unexpected Node for cypher_clause")));

        col = (ColumnRef *)expr;

        if(list_length(col->fields) != 1)
            ereport(ERROR, (errmsg_internal("unexpected Node for cypher_clause")));

        val = linitial(col->fields);

        if(!IsA(val, String))
            ereport(ERROR, (errmsg_internal("unexpected Node for cypher_clause")));

        resno = get_target_entry_resno(query->targetList, val->val.str);

        if (resno == -1)
            ereport(ERROR, (errcode(ERRCODE_INVALID_COLUMN_REFERENCE),
                errmsg("undefined reference to variable %s in DELETE clause", val->val.str),
                parser_errposition(pstate, col->location)));

        pos = makeInteger(resno);

        item->var_name = val->val.str;
        item->entity_position = pos;

        items = lappend(items, item);
    }

    return items;
}

static Query *transform_cypher_set(cypher_parsestate *cpstate,
                                   cypher_clause *clause)
{
    ParseState *pstate = (ParseState *)cpstate;
    cypher_set *self = (cypher_set *)clause->self;
    Query *query;
    cypher_update_information *set_items_target_list;
    TargetEntry *tle;
    Oid func_set_oid;
    Const *pattern_const;
    Expr *func_expr;
    char *clause_name;
    StringInfo str = makeStringInfo();

    query = makeNode(Query);
    query->commandType = CMD_SELECT;
    query->targetList = NIL;

    if (self->is_remove == true)
        clause_name = UPDATE_CLAUSE_REMOVE;
    else
        clause_name = UPDATE_CLAUSE_SET;

    if (!clause->prev)
    {
        ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                        errmsg("%s cannot be the first clause in a Cypher query", clause_name),
                        parser_errposition(pstate, self->location)));
    }
    else
    {
        RangeTblEntry *rte;
        int rtindex;

        rte = transform_prev_cypher_clause(cpstate, clause->prev);
        rtindex = list_length(pstate->p_rtable);
        Assert(rtindex == 1); // rte is the first RangeTblEntry in pstate
        query->targetList = expandRelAttrs(pstate, rte, rtindex, 0, -1);
    }

    func_set_oid = get_ag_func_oid("_cypher_set_clause", 1, INTERNALOID);

    if (list_length(self->items) != 1)
        ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
            errmsg("%s clause does not yet support updating more than one property", clause_name),
            parser_errposition(pstate, self->location)));

    if (self->is_remove == true)
        set_items_target_list = transform_cypher_remove_item_list(cpstate, self->items, query);
    else
        set_items_target_list = transform_cypher_set_item_list(cpstate, self->items, query);

    if (list_length(self->items) != 1)
        ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
            errmsg("%s clause does not yet support updating more than one property",
                    set_items_target_list->clause_name),
            parser_errposition(pstate, self->location)));

    set_items_target_list->clause_name = clause_name;
    set_items_target_list->graph_name = cpstate->graph_name;

    if (!clause->next)
        set_items_target_list->flags |= CYPHER_CLAUSE_FLAG_TERMINAL;

    /*
     * Serialize the cypher_update_information data structure. In certain
     * cases (Prepared Statements and PL/pgsql), the MemoryContext that
     * it is stored in will be destroyed. We need to get it into a format
     * that Postgres' can copy between MemoryContexts. Just making it into
     * an ExtensibleNode does not work, because there are certain parts of
     * Postgres that cannot handle an ExtensibleNode in a function call.
     * So we serialize the data structure and place it into a Const node
     * that can handle these situations AND be copied correctly.
     */
    outNode(str, set_items_target_list);

    pattern_const = makeConst(INTERNALOID, -1, InvalidOid, str->len,
                              PointerGetDatum(str->data), false, false);

    func_expr = (Expr *)makeFuncExpr(func_set_oid, AGTYPEOID,
                                     list_make1(pattern_const), InvalidOid,
                                     InvalidOid, COERCE_EXPLICIT_CALL);

    // Create the target entry
    tle = makeTargetEntry(func_expr, pstate->p_next_resno++,
                          AGE_VARNAME_SET_CLAUSE, false);
    query->targetList = lappend(query->targetList, tle);

    query->rtable = pstate->p_rtable;
    query->jointree = makeFromExpr(pstate->p_joinlist, NULL);

    return query;
}

cypher_update_information *transform_cypher_remove_item_list(
    cypher_parsestate *cpstate, List *remove_item_list, Query *query)
{
    ParseState *pstate = (ParseState *)cpstate;
    ListCell *li;
    cypher_update_information *info = make_ag_node(cypher_update_information);

    info->set_items = NIL;
    info->flags = 0;

    foreach (li, remove_item_list)
    {
        cypher_set_item *set_item = lfirst(li);
        cypher_update_item *item;
        ColumnRef *ref;
        A_Indirection *ind;
        char *variable_name, *property_name;
        Value *property_node, *variable_node;

        item = make_ag_node(cypher_update_item);

        if (!is_ag_node(lfirst(li), cypher_set_item))
            ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                errmsg("unexpected node in cypher update list")));

        if (set_item->is_add)
           ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                errmsg("REMOVE clause does not support adding propereties from maps"),
                parser_errposition(pstate, set_item->location)));

        item->remove_item = true;


        if (!IsA(set_item->prop, A_Indirection))
           ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                errmsg("REMOVE clause must be in the format: REMOVE variable.property_name"),
                parser_errposition(pstate, set_item->location)));

        ind = (A_Indirection *)set_item->prop;

        // extract variable name
        if (!IsA(ind->arg, ColumnRef))
           ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                errmsg("REMOVE clause must be in the format: REMOVE variable.property_name"),
                parser_errposition(pstate, set_item->location)));

        ref = (ColumnRef *)ind->arg;

        variable_node = linitial(ref->fields);

        variable_name = variable_node->val.str;
        item->var_name = variable_name;
        item->entity_position = get_target_entry_resno(query->targetList, variable_name);

        if (item->entity_position == -1)
            ereport(ERROR, (errcode(ERRCODE_INVALID_COLUMN_REFERENCE),
                errmsg("undefined reference to variable %s in REMOVE clause", variable_name),
                parser_errposition(pstate, set_item->location)));

        // extract property name
        if (list_length(ind->indirection) != 1)
           ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                errmsg("REMOVE clause must be in the format: REMOVE variable.property_name"),
                parser_errposition(pstate, set_item->location)));

        property_node = linitial(ind->indirection);


        if (!IsA(property_node, String))
           ereport(ERROR, (errcode(ERRCODE_INVALID_COLUMN_REFERENCE),
                errmsg("REMOVE clause expects a property name"),
                parser_errposition(pstate, set_item->location)));

        property_name = property_node->val.str;
        item->prop_name = property_name;

        info->set_items = lappend(info->set_items, item);
    }

    return info;
}


cypher_update_information *transform_cypher_set_item_list(
    cypher_parsestate *cpstate, List *set_item_list, Query *query)
{
    ParseState *pstate = (ParseState *)cpstate;
    ListCell *li;
    cypher_update_information *info = make_ag_node(cypher_update_information);

    info->set_items = NIL;
    info->flags = 0;

    foreach (li, set_item_list)
    {
        cypher_set_item *set_item = lfirst(li);
        TargetEntry *target_item;
        cypher_update_item *item;
        ColumnRef *ref;
        A_Indirection *ind = (A_Indirection *)set_item->prop;
        char *variable_name, *property_name;
        Value *property_node, *variable_node;

        item = make_ag_node(cypher_update_item);

        if (!is_ag_node(lfirst(li), cypher_set_item))
            ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                errmsg("unexpected node in cypher update list")));

        if (set_item->is_add)
           ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                errmsg("SET clause does not yet support adding propereties from maps"),
                parser_errposition(pstate, set_item->location)));

        item->remove_item = false;

        // extract variable name
        ref = (ColumnRef *)ind->arg;

        variable_node = linitial(ref->fields);
        if (!IsA(variable_node, String))
           ereport(ERROR, (errcode(ERRCODE_INVALID_COLUMN_REFERENCE),
                errmsg("SET clause expects a variable name"),
                parser_errposition(pstate, set_item->location)));

        variable_name = variable_node->val.str;
        item->var_name = variable_name;
        item->entity_position = get_target_entry_resno(query->targetList, variable_name);

        if (item->entity_position == -1)
            ereport(ERROR, (errcode(ERRCODE_INVALID_COLUMN_REFERENCE),
                errmsg("undefined reference to variable %s in SET clause", variable_name),
                parser_errposition(pstate, set_item->location)));

        // extract property name
        if (list_length(ind->indirection) != 1)
           ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                errmsg("SET clause doesnt not support updating maps or lists in a property"),
                parser_errposition(pstate, set_item->location)));

        property_node = linitial(ind->indirection);
        if (!IsA(property_node, String))
           ereport(ERROR, (errcode(ERRCODE_INVALID_COLUMN_REFERENCE),
                errmsg("SET clause expects a property name"),
                parser_errposition(pstate, set_item->location)));

        property_name = property_node->val.str;
        item->prop_name = property_name;

        // create target entry for the new property value
        item->prop_position = (AttrNumber)pstate->p_next_resno;
        target_item = transform_cypher_item(cpstate, set_item->expr, NULL,
                                            EXPR_KIND_SELECT_TARGET, NULL, false);
        target_item->expr = add_volatile_wrapper(target_item->expr);

        query->targetList = lappend(query->targetList, target_item);
        info->set_items = lappend(info->set_items, item);
    }

    return info;
}

/* from PG's static helper function */
static Node *flatten_grouping_sets(Node *expr, bool toplevel,
                                   bool *hasGroupingSets)
{
    /* just in case of pathological input */
    check_stack_depth();

    if (expr == (Node *) NIL)
        return (Node *) NIL;

    switch (expr->type)
    {
        case T_RowExpr:
        {
            RowExpr *r = (RowExpr *) expr;

            if (r->row_format == COERCE_IMPLICIT_CAST)
                return flatten_grouping_sets((Node *) r->args, false, NULL);
            break;
        }
        case T_GroupingSet:
            ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                                    errmsg("flattening of GroupingSet is not implemented")));
            break;
        case T_List:
        {
            List *result = NIL;
            ListCell *l;

            foreach(l, (List *) expr)
            {
                Node *n = flatten_grouping_sets(lfirst(l), toplevel, hasGroupingSets);

                if (n != (Node *) NIL)
                {
                    if (IsA(n, List))
                        result = list_concat(result, (List *) n);
                    else
                        result = lappend(result, n);
                }
            }
            return (Node *) result;
        }
        default:
            break;
    }
    return expr;
}

/* from PG's addTargetToGroupList */
static List *add_target_to_group_list(cypher_parsestate *cpstate,
                                      TargetEntry *tle, List *grouplist,
                                      List *targetlist, int location)
{
    ParseState *pstate = &cpstate->pstate;
    Oid restype = exprType((Node *) tle->expr);

    /* if tlist item is an UNKNOWN literal, change it to TEXT */
    if (restype == UNKNOWNOID)
    {
        tle->expr = (Expr *) coerce_type(pstate, (Node *) tle->expr, restype,
                                         TEXTOID, -1, COERCION_IMPLICIT,
                                         COERCE_IMPLICIT_CAST, -1);
        restype = TEXTOID;
    }

    /* avoid making duplicate grouplist entries */
    if (!targetIsInSortList(tle, InvalidOid, grouplist))
    {
        SortGroupClause *grpcl = makeNode(SortGroupClause);
        Oid sortop;
        Oid eqop;
        bool hashable;
        ParseCallbackState pcbstate;

        setup_parser_errposition_callback(&pcbstate, pstate, location);

        /* determine the eqop and optional sortop */
        get_sort_group_operators(restype, false, true, false, &sortop, &eqop,
                                 NULL, &hashable);

        cancel_parser_errposition_callback(&pcbstate);

        grpcl->tleSortGroupRef = assignSortGroupRef(tle, targetlist);
        grpcl->eqop = eqop;
        grpcl->sortop = sortop;
        grpcl->nulls_first = false; /* OK with or without sortop */
        grpcl->hashable = hashable;

        grouplist = lappend(grouplist, grpcl);
    }

    return grouplist;
}

/* from PG's transformGroupClauseExpr */
static Index transform_group_clause_expr(List **flatresult,
                                         Bitmapset *seen_local,
                                         cypher_parsestate *cpstate,
                                         Node *gexpr, List **targetlist,
                                         List *sortClause,
                                         ParseExprKind exprKind, bool toplevel)
{
    TargetEntry *tle = NULL;
    bool found = false;

    tle = find_target_list_entry(cpstate, gexpr, targetlist, exprKind);

    if (tle->ressortgroupref > 0)
    {
        ListCell *sl;

        /*
         * Eliminate duplicates (GROUP BY x, x) but only at local level.
         * (Duplicates in grouping sets can affect the number of returned
         * rows, so can't be dropped indiscriminately.)
         *
         * Since we don't care about anything except the sortgroupref, we can
         * use a bitmapset rather than scanning lists.
         */
        if (bms_is_member(tle->ressortgroupref, seen_local))
            return 0;

        /*
         * If we're already in the flat clause list, we don't need to consider
         * adding ourselves again.
         */
        found = targetIsInSortList(tle, InvalidOid, *flatresult);
        if (found)
            return tle->ressortgroupref;

        /*
         * If the GROUP BY tlist entry also appears in ORDER BY, copy operator
         * info from the (first) matching ORDER BY item.  This means that if
         * you write something like "GROUP BY foo ORDER BY foo USING <<<", the
         * GROUP BY operation silently takes on the equality semantics implied
         * by the ORDER BY.  There are two reasons to do this: it improves the
         * odds that we can implement both GROUP BY and ORDER BY with a single
         * sort step, and it allows the user to choose the equality semantics
         * used by GROUP BY, should she be working with a datatype that has
         * more than one equality operator.
         *
         * If we're in a grouping set, though, we force our requested ordering
         * to be NULLS LAST, because if we have any hope of using a sorted agg
         * for the job, we're going to be tacking on generated NULL values
         * after the corresponding groups. If the user demands nulls first,
         * another sort step is going to be inevitable, but that's the
         * planner's problem.
         */


         foreach(sl, sortClause)
         {
             SortGroupClause *sc = (SortGroupClause *) lfirst(sl);

             if (sc->tleSortGroupRef == tle->ressortgroupref)
             {
                 SortGroupClause *grpc = copyObject(sc);

                 if (!toplevel)
                     grpc->nulls_first = false;
                 *flatresult = lappend(*flatresult, grpc);
                 found = true;
                 break;
             }
         }
    }

    /*
     * If no match in ORDER BY, just add it to the result using default
     * sort/group semantics.
     */
    if (!found)
        *flatresult = add_target_to_group_list(cpstate, tle, *flatresult,
                                               *targetlist, exprLocation(gexpr));

    /* _something_ must have assigned us a sortgroupref by now... */

    return tle->ressortgroupref;
}

/* from PG's transformGroupClause */
static List * transform_group_clause(cypher_parsestate *cpstate,
                                     List *grouplist, List **groupingSets,
                                     List **targetlist, List *sortClause,
                                     ParseExprKind exprKind)
{
    List *result = NIL;
    List *flat_grouplist;
    List *gsets = NIL;
    ListCell *gl;
    bool hasGroupingSets = false;
    Bitmapset *seen_local = NULL;

    /*
     * Recursively flatten implicit RowExprs. (Technically this is only needed
     * for GROUP BY, per the syntax rules for grouping sets, but we do it
     * anyway.)
     */
    flat_grouplist = (List *) flatten_grouping_sets((Node *) grouplist, true,
                                                    &hasGroupingSets);

    foreach(gl, flat_grouplist)
    {
        Node       *gexpr = (Node *) lfirst(gl);

        if (IsA(gexpr, GroupingSet))
        {
            ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                            errmsg("GroupingSet is not implemented")));
            break;
        }
        else
        {
            Index ref = transform_group_clause_expr(&result, seen_local,
                                                    cpstate, gexpr, targetlist,
                                                    sortClause, exprKind, true);
            if (ref > 0)
            {
                seen_local = bms_add_member(seen_local, ref);
                if (hasGroupingSets)
                    ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                            errmsg("GroupingSet is not implemented")));
            }
        }
    }

    /* parser should prevent this */
    Assert(gsets == NIL || groupingSets != NULL);

    if (groupingSets)
        *groupingSets = gsets;

    return result;
}

static Query *transform_cypher_return(cypher_parsestate *cpstate,
                                      cypher_clause *clause)
{
    ParseState *pstate = (ParseState *)cpstate;
    cypher_return *self = (cypher_return *)clause->self;
    Query *query;
    List *groupClause = NIL;

    query = makeNode(Query);
    query->commandType = CMD_SELECT;

    if (clause->prev)
        transform_prev_cypher_clause(cpstate, clause->prev);

    query->targetList = transform_cypher_item_list(cpstate, self->items,
                                                   &groupClause,
                                                   EXPR_KIND_SELECT_TARGET);

    markTargetListOrigins(pstate, query->targetList);

    // ORDER BY
    query->sortClause = transform_cypher_order_by(cpstate, self->order_by,
                                                  &query->targetList,
                                                  EXPR_KIND_ORDER_BY);

    /* 'auto' GROUP BY (from PG's transformGroupClause) */
    query->groupClause = transform_group_clause(cpstate, groupClause,
                                                &query->groupingSets,
                                                &query->targetList,
                                                query->sortClause,
                                                EXPR_KIND_GROUP_BY);

    // DISTINCT
    if (self->distinct)
    {
        query->distinctClause = transformDistinctClause(
            pstate, &query->targetList, query->sortClause, false);
        query->hasDistinctOn = false;
    }
    else
    {
        query->distinctClause = NIL;
        query->hasDistinctOn = false;
    }

    // SKIP and LIMIT
    query->limitOffset = transform_cypher_limit(cpstate, self->skip,
                                                EXPR_KIND_OFFSET, "SKIP");
    query->limitCount = transform_cypher_limit(cpstate, self->limit,
                                               EXPR_KIND_LIMIT, "LIMIT");

    query->rtable = pstate->p_rtable;
    query->jointree = makeFromExpr(pstate->p_joinlist, NULL);
    query->hasAggs = pstate->p_hasAggs;

    assign_query_collations(pstate, query);

    /* this must be done after collations, for reliable comparison of exprs */
    if (pstate->p_hasAggs ||
        query->groupClause || query->groupingSets || query->havingQual)
        parse_check_aggregates(pstate, query);

    return query;
}

// see transformSortClause()
static List *transform_cypher_order_by(cypher_parsestate *cpstate,
                                       List *sort_items, List **target_list,
                                       ParseExprKind expr_kind)
{
    ParseState *pstate = (ParseState *)cpstate;
    List *sort_list = NIL;
    ListCell *li;

    foreach (li, sort_items)
    {
        SortBy *sort_by = lfirst(li);
        TargetEntry *te;

        te = find_target_list_entry(cpstate, sort_by->node, target_list,
                                    expr_kind);

        sort_list = addTargetToSortList(pstate, te, sort_list, *target_list,
                                        sort_by);
    }

    return sort_list;
}

// see findTargetlistEntrySQL99()
static TargetEntry *find_target_list_entry(cypher_parsestate *cpstate,
                                           Node *node, List **target_list,
                                           ParseExprKind expr_kind)
{
    Node *expr;
    ListCell *lt;
    TargetEntry *te;

    expr = transform_cypher_expr(cpstate, node, expr_kind);

    foreach (lt, *target_list)
    {
        Node *te_expr;

        te = lfirst(lt);
        te_expr = strip_implicit_coercions((Node *)te->expr);

        if (equal(expr, te_expr))
            return te;
    }

    te = transform_cypher_item(cpstate, node, expr, expr_kind, NULL, true);

    *target_list = lappend(*target_list, te);

    return te;
}

// see transformLimitClause()
static Node *transform_cypher_limit(cypher_parsestate *cpstate, Node *node,
                                    ParseExprKind expr_kind,
                                    const char *construct_name)
{
    ParseState *pstate = (ParseState *)cpstate;
    Node *qual;

    if (!node)
        return NULL;

    qual = transform_cypher_expr(cpstate, node, expr_kind);

    qual = coerce_to_specific_type(pstate, qual, INT8OID, construct_name);

    // LIMIT can't refer to any variables of the current query.
    if (contain_vars_of_level(qual, 0))
    {
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_COLUMN_REFERENCE),
                 errmsg("argument of %s must not contain variables",
                        construct_name),
                 parser_errposition(pstate, locate_var_of_level(qual, 0))));
    }

    return qual;
}

static Query *transform_cypher_with(cypher_parsestate *cpstate,
                                    cypher_clause *clause)
{
    cypher_with *self = (cypher_with *)clause->self;
    cypher_return *return_clause;
    cypher_clause *wrapper;

    // TODO: check that all items have an alias for each

    // WITH clause is basically RETURN clause with optional WHERE subclause
    return_clause = make_ag_node(cypher_return);
    return_clause->distinct = self->distinct;
    return_clause->items = self->items;
    return_clause->order_by = self->order_by;
    return_clause->skip = self->skip;
    return_clause->limit = self->limit;

    wrapper = palloc(sizeof(*wrapper));
    wrapper->self = (Node *)return_clause;
    wrapper->prev = clause->prev;

    return transform_cypher_clause_with_where(cpstate, transform_cypher_return,
                                              wrapper, self->where);
}

static Query *transform_cypher_clause_with_where(cypher_parsestate *cpstate,
                                                 transform_method transform,
                                                 cypher_clause *clause,
                                                 Node *where)
{
    ParseState *pstate = (ParseState *)cpstate;
    Query *query;

    if (where)
    {
        RangeTblEntry *rte;
        int rtindex;
        Node *qual;

        query = makeNode(Query);
        query->commandType = CMD_SELECT;

        rte = transform_cypher_clause_as_subquery(cpstate, transform, clause);

        rtindex = list_length(pstate->p_rtable);
        Assert(rtindex == 1); // rte is the only RangeTblEntry in pstate

        query->targetList = expandRelAttrs(pstate, rte, rtindex, 0, -1);

        markTargetListOrigins(pstate, query->targetList);

        // see transformWhereClause()
        qual = transform_cypher_expr(cpstate, where, EXPR_KIND_WHERE);
        qual = coerce_to_boolean(pstate, qual, "WHERE");

        query->rtable = pstate->p_rtable;
        query->jointree = makeFromExpr(pstate->p_joinlist, qual);

        assign_query_collations(pstate, query);
    }
    else
    {
        query = transform(cpstate, clause);
    }

    query->hasSubLinks = pstate->p_hasSubLinks;
    query->hasTargetSRFs = pstate->p_hasTargetSRFs;
    query->hasAggs = pstate->p_hasAggs;

    return query;
}

static Query *transform_cypher_match(cypher_parsestate *cpstate,
                                     cypher_clause *clause)
{
    cypher_match *self = (cypher_match *)clause->self;

    return transform_cypher_clause_with_where(
        cpstate, transform_cypher_match_pattern, clause, self->where);
}

static Query *transform_cypher_match_pattern(cypher_parsestate *cpstate,
                                             cypher_clause *clause)
{
    ParseState *pstate = (ParseState *)cpstate;
    cypher_match *self = (cypher_match *)clause->self;
    Query *query;

    query = makeNode(Query);
    query->commandType = CMD_SELECT;

    if (clause->prev)
    {
        RangeTblEntry *rte;
        int rtindex;

        rte = transform_prev_cypher_clause(cpstate, clause->prev);
        rtindex = list_length(pstate->p_rtable);
        Assert(rtindex == 1); // rte is the first RangeTblEntry in pstate

        /*
         * add all the target entries in rte to the current target list to pass
         * all the variables that are introduced in the previous clause to the
         * next clause
         */
        query->targetList = expandRelAttrs(pstate, rte, rtindex, 0, -1);
    }

    transform_match_pattern(cpstate, query, self->pattern);

    markTargetListOrigins(pstate, query->targetList);

    assign_query_collations(pstate, query);

    return query;
}

/*
 * Function to make a target list from an RTE. Borrowed from AgensGraph and PG
 */
static List *makeTargetListFromRTE(ParseState *pstate, RangeTblEntry *rte)
{
    List *targetlist = NIL;
    int rtindex;
    int varattno;
    ListCell *ln;
    ListCell *lt;

    /* right now this is only for subqueries */
    AssertArg(rte->rtekind == RTE_SUBQUERY);

    rtindex = RTERangeTablePosn(pstate, rte, NULL);

    varattno = 1;
    ln = list_head(rte->eref->colnames);
    foreach(lt, rte->subquery->targetList)
    {
        TargetEntry *te = lfirst(lt);
        Var *varnode;
        char *resname;
        TargetEntry *tmp;

        if (te->resjunk)
            continue;

        Assert(varattno == te->resno);

        /* no transform here, just use `te->expr` */
        varnode = makeVar(rtindex, varattno, exprType((Node *) te->expr),
                          exprTypmod((Node *) te->expr),
                          exprCollation((Node *) te->expr), 0);

        resname = strVal(lfirst(ln));

        tmp = makeTargetEntry((Expr *)varnode,
                              (AttrNumber)pstate->p_next_resno++, resname,
                              false);
        targetlist = lappend(targetlist, tmp);

        varattno++;
        ln = lnext(ln);
    }

    return targetlist;
}

/*
 * Transform a cypher sub pattern. This is put here because it is a sub clause.
 * This works in tandem with transform_Sublink in cypher_expr.c
 */
static Query *transform_cypher_sub_pattern(cypher_parsestate *cpstate,
                                           cypher_clause *clause)
{
    cypher_match *match;
    cypher_clause *c;
    Query *qry;
    RangeTblEntry *rte;
    ParseState *pstate = (ParseState *)cpstate;
    cypher_sub_pattern *subpat = (cypher_sub_pattern*)clause->self;

    /* create a cypher match node and assign it the sub pattern */
    match = make_ag_node(cypher_match);
    match->pattern = subpat->pattern;
    match->where = NULL;
    /* wrap it in a clause */
    c = palloc(sizeof(cypher_clause));
    c->self = (Node *)match;
    c->prev = NULL;
    c->next = NULL;

    /* set up a select query and run it as a sub query to the parent match */
    qry = makeNode(Query);
    qry->commandType = CMD_SELECT;

    rte = transform_cypher_clause_as_subquery(cpstate, transform_cypher_clause,
                                              c);

    qry->targetList = makeTargetListFromRTE(pstate, rte);

    markTargetListOrigins(pstate, qry->targetList);

    qry->rtable = pstate->p_rtable;
    qry->jointree = makeFromExpr(pstate->p_joinlist, NULL);

    /* the state will be destroyed so copy the data we need */
    qry->hasSubLinks = pstate->p_hasSubLinks;
    qry->hasTargetSRFs = pstate->p_hasTargetSRFs;
    qry->hasAggs = pstate->p_hasAggs;

    if (qry->hasAggs)
        parse_check_aggregates(pstate, qry);

    assign_query_collations(pstate, qry);

    return qry;
}

static void transform_match_pattern(cypher_parsestate *cpstate, Query *query,
                                    List *pattern)
{
    ListCell *lc;
    List *quals = NIL;
    Expr *q = NULL;
    Expr *expr = NULL;

    foreach (lc, pattern)
    {
        cypher_path *path = lfirst(lc);
        List *qual = transform_match_path(cpstate, query, path);

        quals = list_concat(quals, qual);
    }

    if (quals != NIL)
    {
        q = makeBoolExpr(AND_EXPR, quals, -1);
        expr = (Expr *)transformExpr(&cpstate->pstate, (Node *)q, EXPR_KIND_WHERE);
    }

    if (cpstate->property_constraint_quals != NIL)
    {
        Expr *prop_qual = makeBoolExpr(AND_EXPR, cpstate->property_constraint_quals, -1);

        if (quals == NIL)
            expr = prop_qual;
        else
            expr = makeBoolExpr(AND_EXPR, list_make2(expr, prop_qual), -1);
    }


    query->rtable = cpstate->pstate.p_rtable;
    query->jointree = makeFromExpr(cpstate->pstate.p_joinlist, (Node *)expr);
}

static char *get_next_default_alias(cypher_parsestate *cpstate)
{
    int base_length;
    char alias_num[12];
    char *alias;

    sprintf(alias_num, "%d", cpstate->default_alias_num++);

    base_length = strlen(AGE_DEFAULT_ALIAS_PREFIX);

    alias = palloc0(sizeof(char) * (12 + base_length));

    strncat(alias, AGE_DEFAULT_ALIAS_PREFIX, base_length);

    strncat(alias + base_length, alias_num, 12);

    return alias;
}

/*
 * Creates a FuncCall node that will prevent an edge from being joined
 * to twice.
 */
static FuncCall *prevent_duplicate_edges(cypher_parsestate *cpstate,
                                         List *entities)
{
    List *edges = NIL;
    ListCell *lc;
    List *qualified_function_name;
    Value *ag_catalog, *edge_fn;

    ag_catalog = makeString("ag_catalog");
    edge_fn = makeString("_ag_enforce_edge_uniqueness");

    qualified_function_name = list_make2(ag_catalog, edge_fn);

    // iterate through each entity, collecting the access node for each edge
    foreach (lc, entities)
    {
        transform_entity *entity = lfirst(lc);
        FuncCall *edge;

        // skip vertices
        if (entity->type != ENT_EDGE)
            continue;

        edge = make_qual(cpstate, entity, AG_EDGE_COLNAME_ID);

        edges = lappend(edges, edge);
    }

    return makeFuncCall(qualified_function_name, edges, -1);
}

/*
 * For any given edge, the previous entity is joined with the edge
 * via the prev_qual node, and the next entity is join with the
 * next_qual node. If there is a filter on the previous vertex label,
 * create a filter, same with the next node.
 */
static List *make_directed_edge_join_conditions(
    cypher_parsestate *cpstate, transform_entity *prev_entity,
    transform_entity *next_entity, FuncCall *prev_qual, FuncCall *next_qual,
    char *prev_node_filter, char *next_node_filter)
{
    List *quals = NIL;

    if (prev_entity->in_join_tree)
        quals = list_concat(quals, join_to_entity(cpstate, prev_entity,
                                                  prev_qual, JOIN_SIDE_LEFT));

    if (next_entity->in_join_tree)
        quals = list_concat(quals, join_to_entity(cpstate, next_entity,
                                                  next_qual, JOIN_SIDE_RIGHT));

    if (prev_node_filter != NULL && !IS_DEFAULT_LABEL_VERTEX(prev_node_filter))
    {
        A_Expr *qual;
        qual = filter_vertices_on_label_id(cpstate, prev_qual,
                                           prev_node_filter);

        quals = lappend(quals, qual);
    }

    if (next_node_filter != NULL && !IS_DEFAULT_LABEL_VERTEX(next_node_filter))
    {
        A_Expr *qual;
        qual = filter_vertices_on_label_id(cpstate, next_qual,
                                           next_node_filter);

        quals = lappend(quals, qual);
    }

    return quals;
}

/*
 * The joins are driven by edges. Under specific conditions, it becomes
 * necessary to have knowledge about the previous edge and vertex and
 * the next vertex and edge.
 *
 * [prev_edge]-(prev_node)-[edge]-(next_node)-[next_edge]
 *
 * prev_edge and next_edge are allowed to be null.
 * prev_node and next_node are not allowed to be null.
 */
static List *make_join_condition_for_edge(cypher_parsestate *cpstate,
                                          transform_entity *prev_edge,
                                          transform_entity *prev_node,
                                          transform_entity *entity,
                                          transform_entity *next_node,
                                          transform_entity *next_edge)
{
    char *next_label_name_to_filter = NULL;
    char *prev_label_name_to_filter = NULL;
    transform_entity *next_entity;
    transform_entity *prev_entity;

    /*
     *  If the previous node is not in the join tree, set the previous
     *  label filter.
     */
    if (!prev_node->in_join_tree)
        prev_label_name_to_filter = prev_node->entity.node->label;

    /*
     * If the next node is not in the join tree and there is not
     * another edge, set the label filter. When there is another
     * edge, we don't need to set it, because that edge will set the
     * filter for that node.
     */
    if (!next_node->in_join_tree && next_edge == NULL)
        next_label_name_to_filter = next_node->entity.node->label;

    /*
     * When the previous node is not in the join tree, and there
     * is a previous edge, set the previous entity to that edge.
     * Otherwise, use the previous node/
     */
    if (!prev_node->in_join_tree && prev_edge != NULL)
        prev_entity = prev_edge;
    else
        prev_entity = prev_node;

    /*
     * When the next node is not in the join tree, and there
     * is a next edge, set the next entity to that edge.
     * Otherwise, use the next node.
     */
    if (!next_node->in_join_tree && next_edge != NULL)
        next_entity = next_edge;
    else
        next_entity = next_node;

    switch (entity->entity.rel->dir)
    {
    case CYPHER_REL_DIR_RIGHT:
    {
        FuncCall *prev_qual = make_qual(cpstate, entity,
                                        AG_EDGE_COLNAME_START_ID);
        FuncCall *next_qual = make_qual(cpstate, entity,
                                        AG_EDGE_COLNAME_END_ID);

        return make_directed_edge_join_conditions(
            cpstate, prev_entity, next_node, prev_qual, next_qual,
            prev_label_name_to_filter, next_label_name_to_filter);
    }
    case CYPHER_REL_DIR_LEFT:
    {
        FuncCall *prev_qual = make_qual(cpstate, entity,
                                        AG_EDGE_COLNAME_END_ID);
        FuncCall *next_qual = make_qual(cpstate, entity,
                                        AG_EDGE_COLNAME_START_ID);

        return make_directed_edge_join_conditions(
            cpstate, prev_entity, next_node, prev_qual, next_qual,
            prev_label_name_to_filter, next_label_name_to_filter);
    }
    case CYPHER_REL_DIR_NONE:
    {
        /*
         * For undirected relationships, we can use the left directed
         * relationship OR'd by the right directed relationship.
         */
        FuncCall *start_id_expr = make_qual(cpstate, entity,
                                            AG_EDGE_COLNAME_START_ID);
        FuncCall *end_id_expr = make_qual(cpstate, entity,
                                          AG_EDGE_COLNAME_END_ID);
        List *first_join_quals = NIL, *second_join_quals = NIL;
        Expr *first_qual, *second_qual;
        Expr *or_qual;

        first_join_quals = make_directed_edge_join_conditions(
            cpstate, prev_entity, next_entity, start_id_expr, end_id_expr,
            prev_label_name_to_filter, next_label_name_to_filter);

        second_join_quals = make_directed_edge_join_conditions(
            cpstate, prev_entity, next_entity, end_id_expr, start_id_expr,
            prev_label_name_to_filter, next_label_name_to_filter);

        first_qual = makeBoolExpr(AND_EXPR, first_join_quals, -1);
        second_qual = makeBoolExpr(AND_EXPR, second_join_quals, -1);

        or_qual = makeBoolExpr(OR_EXPR, list_make2(first_qual, second_qual),
                               -1);

        return list_make1(or_qual);
    }
    default:
        return NULL;
    }
}

/*
 * For the given entity, join it to the current edge, via the passed
 * qual node. The side denotes if the entity is on the right
 * or left of the current edge. Which we will need to know if the
 * passed entity is a directed edge.
 */
static List *join_to_entity(cypher_parsestate *cpstate,
                            transform_entity *entity, FuncCall *qual,
                            enum transform_entity_join_side side)
{
    A_Expr *expr;
    List *quals = NIL;

    if (entity->type == ENT_VERTEX)
    {
        FuncCall *id_qual = make_qual(cpstate, entity, AG_EDGE_COLNAME_ID);

        expr = makeSimpleA_Expr(AEXPR_OP, "=", (Node *)qual, (Node *)id_qual,
                                -1);

        quals = lappend(quals, expr);
    }
    else if (entity->type == ENT_EDGE)
    {
        List *edge_quals = make_edge_quals(cpstate, entity, side);

        if (list_length(edge_quals) > 1)
            expr = makeSimpleA_Expr(AEXPR_IN, "=", (Node *)qual,
                                    (Node *)edge_quals, -1);
        else
            expr = makeSimpleA_Expr(AEXPR_OP, "=", (Node *)qual,
                                    linitial(edge_quals), -1);

        quals = lappend(quals, expr);
    }
    else
        ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                        errmsg("unknown entity type to join to")));

    return quals;
}

// makes the quals neccessary when an edge is joining to another edge.
static List *make_edge_quals(cypher_parsestate *cpstate,
                             transform_entity *edge,
                             enum transform_entity_join_side side)
{
    ParseState *pstate = (ParseState *)cpstate;
    char *left_dir;
    char *right_dir;

    Assert(edge->type == ENT_EDGE);

    /*
     * When the rel is on the left side in a pattern, then a left directed path
     * is concerned with the start id and a right directed path is concerned
     * with the end id. When the rel is on the right side of a pattern, the
     * above statement is inverted.
     */
    switch (side)
    {
    case JOIN_SIDE_LEFT:
    {
        left_dir = AG_EDGE_COLNAME_START_ID;
        right_dir = AG_EDGE_COLNAME_END_ID;
        break;
    }
    case JOIN_SIDE_RIGHT:
    {
        left_dir = AG_EDGE_COLNAME_END_ID;
        right_dir = AG_EDGE_COLNAME_START_ID;
        break;
    }
    default:
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("unknown join type found"),
                 parser_errposition(pstate, edge->entity.rel->location)));
    }

    switch (edge->entity.rel->dir)
    {
    case CYPHER_REL_DIR_LEFT:
    {
        return list_make1(make_qual(cpstate, edge, left_dir));
    }
    case CYPHER_REL_DIR_RIGHT:
    {
        return list_make1(make_qual(cpstate, edge, right_dir));
    }
    case CYPHER_REL_DIR_NONE:
    {
        return list_make2(make_qual(cpstate, edge, left_dir),
                          make_qual(cpstate, edge, right_dir));
    }
    default:
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("Unknown relationship direction")));
    }
    return NIL;
}

/*
 * Creates a node that will create a filter on the passed field node
 * that removes all labels that do not have the same label_id
 */
static A_Expr *filter_vertices_on_label_id(cypher_parsestate *cpstate,
                                           FuncCall *id_field, char *label)
{
    label_cache_data *lcd = search_label_name_graph_cache(label,
                                                          cpstate->graph_oid);
    A_Const *n;
    FuncCall *fc, *conversion_fc;
    Value *ag_catalog, *extract_label_id, *agtype_to_graphid;
    int32 label_id = lcd->id;

    n = makeNode(A_Const);
    n->val.type = T_Integer;
    n->val.val.ival = label_id;
    n->location = -1;

    ag_catalog = makeString("ag_catalog");
    extract_label_id = makeString("_extract_label_id");
    agtype_to_graphid = makeString("agtype_to_graphid");

    conversion_fc = makeFuncCall(list_make2(ag_catalog, agtype_to_graphid),
                                 list_make1(id_field), -1);

    fc = makeFuncCall(list_make2(ag_catalog, extract_label_id),
                      list_make1(conversion_fc), -1);

    return makeSimpleA_Expr(AEXPR_OP, "=", (Node *)fc, (Node *)n, -1);
}

static transform_entity *make_transform_entity(cypher_parsestate *cpstate,
                                               enum transform_entity_type type,
                                               Node *node, Expr *expr)
{
    transform_entity *entity;

    entity = palloc(sizeof(transform_entity));

    entity->type = type;
    if (type == ENT_VERTEX)
        entity->entity.node = (cypher_node *)node;
    else if (entity->type == ENT_EDGE)
        entity->entity.rel = (cypher_relationship *)node;
    else
        ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                        errmsg("unknown entity type")));

    entity->declared_in_current_clause = true;
    entity->expr = expr;
    entity->in_join_tree = expr != NULL;

    return entity;
}

static transform_entity *find_variable(cypher_parsestate *cpstate, char *name)
{
    ListCell *lc;

    foreach (lc, cpstate->entities)
    {
        transform_entity *entity = lfirst(lc);
        char *entity_name;

        if (entity->type == ENT_VERTEX)
            entity_name = entity->entity.node->name;
        else if (entity->type == ENT_EDGE)
            entity_name = entity->entity.rel->name;
        else
            ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                            errmsg("unknown entity type")));

        if (entity_name != NULL && !strcmp(name, entity_name))
            return entity;
    }

    return NULL;
}

/*
 * Create a function to handle property constraints on an edge/vertex.
 * Since the property constraints might be a parameter, we cannot split
 * the property map into indvidual quals, this will be slightly inefficient,
 * but necessary to cover all possible situations.
 */
static Node *create_property_constraint_function(cypher_parsestate *cpstate,
                                                 transform_entity *entity,
                                                 Node *property_constraints)
{
    ParseState *pstate = (ParseState *)cpstate;
    char *entity_name;
    ColumnRef *cr;
    FuncExpr *fexpr;
    Oid func_oid;
    Node *prop_expr, *const_expr;
    RangeTblEntry *rte;

    cr = makeNode(ColumnRef);

    if (entity->type == ENT_EDGE)
        entity_name = entity->entity.node->name;
    else if (entity->type == ENT_VERTEX)
        entity_name = entity->entity.rel->name;
    else
	ereport(ERROR,
            (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
             errmsg("cannot create a property constraint on non vertex or edge agtype")));

    cr->fields = list_make2(makeString(entity_name), makeString("properties"));

    // use Postgres to get the properties' transform node
    if ((rte = find_rte(cpstate, entity_name)))
        prop_expr = scanRTEForColumn(pstate, rte, AG_VERTEX_COLNAME_PROPERTIES,
                                     -1, 0, NULL);
    else
        prop_expr = transformExpr(pstate, (Node *)cr, EXPR_KIND_WHERE);

    // use cypher to get the constraints' transform node
    const_expr = transform_cypher_expr(cpstate, property_constraints,
                                       EXPR_KIND_WHERE);

    func_oid = get_ag_func_oid("_property_constraint_check", 2, AGTYPEOID,
                               AGTYPEOID);

    fexpr = makeFuncExpr(func_oid, BOOLOID, list_make2(prop_expr, const_expr),
                         InvalidOid, InvalidOid, COERCE_EXPLICIT_CALL);

    return (Node *)fexpr;
}


/*
 * For the given path, transform each entity within the path, create
 * the path variable if needed, and construct the quals to enforce the
 * correct join tree, and enforce edge uniqueness.
 */
static List *transform_match_path(cypher_parsestate *cpstate, Query *query,
                                  cypher_path *path)
{
    List *qual = NIL;
    List *entities = NIL;
    FuncCall *duplicate_edge_qual;
    List *join_quals;

    // transform the entities in the path
    entities = transform_match_entities(cpstate, query, path);

    // create the path variable, if needed.
    if (path->var_name != NULL)
    {
        TargetEntry *path_te;

        path_te = transform_match_create_path_variable(cpstate, path,
                                                       entities);
        query->targetList = lappend(query->targetList, path_te);
    }

    // construct the quals for the join tree
    join_quals = make_path_join_quals(cpstate, entities);
    qual = list_concat(qual, join_quals);

    // construct the qual to prevent duplicate edges
    if (list_length(entities) > 3)
    {
        duplicate_edge_qual = prevent_duplicate_edges(cpstate, entities);
        qual = lappend(qual, duplicate_edge_qual);
    }

    return qual;
}

/*
 * Iterate through the path and construct all edges and necessary vertices
 */
static List *transform_match_entities(cypher_parsestate *cpstate, Query *query,
                                      cypher_path *path)
{
    int i = 0;
    ListCell *lc;
    List *entities = NIL;

    /*
     * Iterate through every node in the path, construct the expr node
     * that is needed for the remaining steps
     */
    foreach (lc, path->path)
    {
        Expr *expr;
        transform_entity *entity;

        if (i % 2 == 0)
        {
            cypher_node *node = lfirst(lc);

            expr =
                transform_cypher_node(cpstate, node, &query->targetList,
                                      INCLUDE_NODE_IN_JOIN_TREE(path, node));

            entity = make_transform_entity(cpstate, ENT_VERTEX, (Node *)node,
                                           expr);

            cpstate->entities = lappend(cpstate->entities, entity);

            if (node->props)
            {
                Node *n = create_property_constraint_function(cpstate, entity, node->props);
                cpstate->property_constraint_quals = lappend(cpstate->property_constraint_quals, n);
            }

            entities = lappend(entities, entity);
        }
        else
        {
            cypher_relationship *rel = lfirst(lc);

            expr = transform_cypher_edge(cpstate, rel, &query->targetList);

            entity = make_transform_entity(cpstate, ENT_EDGE, (Node *)rel,
                                           expr);

            cpstate->entities = lappend(cpstate->entities, entity);

            if (rel->props)
            {
                Node *n = create_property_constraint_function(cpstate, entity, rel->props);
                cpstate->property_constraint_quals = lappend(cpstate->property_constraint_quals, n);
            }

            entities = lappend(entities, entity);
        }

        i++;
    }

    return entities;
}

/*
 * Iterate through the list of entities setup the join conditions. Joins
 * are driven through edges. To correctly setup the joins, we must
 * aquire information about the previous edge and vertex, and the next
 * edge and vertex.
 */
static List *make_path_join_quals(cypher_parsestate *cpstate, List *entities)
{
    transform_entity *prev_node = NULL, *prev_edge = NULL, *edge = NULL,
                     *next_node = NULL, *next_edge = NULL;
    ListCell *lc;
    List *quals = NIL;
    List *join_quals;

    // for vertex only queries, there is no work to do
    if (list_length(entities) < 3)
        return NIL;

    lc = list_head(entities);
    for (;;)
    {
        /*
         * Initial setup, set the initial vertex as the previous vertex
         * and get the first edge
         */
        if (prev_node == NULL)
        {
            prev_node = lfirst(lc);
            lc = lnext(lc);
            edge = lfirst(lc);
        }

        // Retrieve the next node and edge in the pattern.
        if (lnext(lc) != NULL)
        {
            lc = lnext(lc);
            next_node = lfirst(lc);

            if (lnext(lc) != NULL)
            {
                lc = lnext(lc);
                next_edge = lfirst(lc);
            }
        }

        // create the join quals for the node
        join_quals = make_join_condition_for_edge(
            cpstate, prev_edge, prev_node, edge, next_node, next_edge);

        quals = list_concat(quals, join_quals);

        /* Set the edge as the previous edge and the next edge as
         * the current edge. If there is not a new edge, exit the
         * for loop.
         */
        prev_edge = edge;
        prev_node = next_node;
        edge = next_edge;
        next_node = NULL;
        next_edge = NULL;

        if (edge == NULL)
            return quals;
    }
}

/*
 * Create the path variable. Takes the list of entities, extracts the variable
 * and passes as the arguement list for the _agtype_build_path function.
 */
static TargetEntry *
transform_match_create_path_variable(cypher_parsestate *cpstate,
                                     cypher_path *path, List *entities)
{
    ParseState *pstate = (ParseState *)cpstate;
    Oid build_path_oid;
    FuncExpr *fexpr;
    int resno;
    List *entity_exprs = NIL;
    ListCell *lc;

    if (list_length(entities) < 3)
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("paths consist of alternating vertices and edges."),
                 parser_errposition(pstate, path->location),
                 errhint("paths require at least 2 vertices and 1 edge")));

    // extract the expr for each entity
    foreach (lc, entities)
    {
        transform_entity *entity = lfirst(lc);

        entity_exprs = lappend(entity_exprs, entity->expr);
    }

    // get the oid for the path creation function
    build_path_oid = get_ag_func_oid("_agtype_build_path", 1, ANYOID);

    // build the expr node for the function
    fexpr = makeFuncExpr(build_path_oid, AGTYPEOID, entity_exprs, InvalidOid,
                         InvalidOid, COERCE_EXPLICIT_CALL);

    resno = cpstate->pstate.p_next_resno++;

    // create the target entry
    return makeTargetEntry((Expr *)fexpr, resno, path->var_name, false);
}

/*
 * Maps a column name to the a function access name. In others word when
 * passed the name for the vertex's id column name, return the function name
 * for the vertex's agtype id element, etc.
 */
static char *get_accessor_function_name(enum transform_entity_type type,
                                        char *name)
{
    if (type == ENT_VERTEX)
    {
        // id
        if (!strcmp(AG_VERTEX_COLNAME_ID, name))
            return AG_VERTEX_ACCESS_FUNCTION_ID;
        // props
        else if(!strcmp(AG_VERTEX_COLNAME_PROPERTIES, name))
            return AG_VERTEX_ACCESS_FUNCTION_PROPERTIES;

    }
    if (type == ENT_EDGE)
    {
        // id
        if (!strcmp(AG_EDGE_COLNAME_ID, name))
            return AG_EDGE_ACCESS_FUNCTION_ID;
        // start id
        else if (!strcmp(AG_EDGE_COLNAME_START_ID, name))
            return AG_EDGE_ACCESS_FUNCTION_START_ID;
        // end id
        else if (!strcmp(AG_EDGE_COLNAME_END_ID, name))
            return AG_EDGE_ACCESS_FUNCTION_END_ID;
        // props
        else if(!strcmp(AG_VERTEX_COLNAME_PROPERTIES, name))
            return AG_VERTEX_ACCESS_FUNCTION_PROPERTIES;
    }

    ereport(ERROR,
            (errcode(ERRCODE_INVALID_COLUMN_REFERENCE),
             errmsg("column %s does not have an accessor function", name)));
    // keeps compiler silent
    return NULL;
}

/*
 * For the given entity and column name, construct an expression that will access
 * the column or get the access function if the entity is a variable.
 */
static FuncCall *make_qual(cypher_parsestate *cpstate,
                           transform_entity *entity, char *col_name)
{
    List *qualified_name, *args;

    if (IsA(entity->expr, Var))
    {
        char *function_name;

        function_name = get_accessor_function_name(entity->type, col_name);

        qualified_name = list_make2(makeString("ag_catalog"),
                                    makeString(function_name));

        args = list_make1(entity->expr);
    }
    else
    {
        char *entity_name;
        ColumnRef *cr = makeNode(ColumnRef);

        // cast graphid to agtype
        qualified_name = list_make2(makeString("ag_catalog"),
                                    makeString("graphid_to_agtype"));

        if (entity->type == ENT_EDGE)
            entity_name = entity->entity.node->name;
        else if (entity->type == ENT_VERTEX)
            entity_name = entity->entity.rel->name;
        else
            ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                            errmsg("unknown entity type")));

        cr->fields = list_make2(makeString(entity_name), makeString(col_name));

        args = list_make1(cr);
    }

    return makeFuncCall(qualified_name, args, -1);
}

static Expr *transform_cypher_edge(cypher_parsestate *cpstate,
                                   cypher_relationship *rel,
                                   List **target_list)
{
    ParseState *pstate = (ParseState *)cpstate;
    char *schema_name;
    char *rel_name;
    RangeVar *label_range_var;
    Alias *alias;
    RangeTblEntry *rte;
    int resno;
    TargetEntry *te;
    Expr *expr;

    if (rel->varlen != NULL)
    {
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("variable length relationships are not supported"),
                 parser_errposition(pstate, rel->location)));
    }

    if (!rel->label)
        rel->label = AG_DEFAULT_LABEL_EDGE;
    else
    {
        /*
         *  XXX: Need to determine proper rules, for when label does not exist
         *  or is for an edge. Maybe labels and edges should share names, like
         *  in openCypher. But these are stand in errors, to prevent segmentation
         *  faults, and other errors.
         */
        label_cache_data *lcd =
            search_label_name_graph_cache(rel->label, cpstate->graph_oid);

        if (lcd == NULL)
            ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                            errmsg("label %s does not exists", rel->label),
                            parser_errposition(pstate, rel->location)));

        if (lcd->kind != LABEL_KIND_EDGE)
            ereport(ERROR,
                    (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                     errmsg("label %s is for vertices, not edges", rel->label),
                     parser_errposition(pstate, rel->location)));
    }

    if (rel->name != NULL)
    {
        TargetEntry *te = findTarget(*target_list, rel->name);
        /* also search for a variable from a previous transform */
        Node *expr = colNameToVar(pstate, rel->name, false, rel->location);

        if (expr != NULL)
            return (Expr*)expr;

        if (te != NULL)
        {
            transform_entity *entity = find_variable(cpstate, rel->name);

            /*
             * TODO: openCypher allows a variable to be used before it
             * is properly declared. This logic is not satifactory
             * for that and must be better developed.
             */
            if (entity != NULL &&
                (entity->type != ENT_EDGE ||
                 !IS_DEFAULT_LABEL_EDGE(rel->label) ||
                 rel->props))
                ereport(ERROR,
                        (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                         errmsg("variable %s already exists", rel->name),
                         parser_errposition(pstate, rel->location)));

            return te->expr;
        }

        /*
         * If we are in a WHERE clause transform, we don't want to create new
         * variables, we want to use the existing ones. So, error if otherwise.
         */
        if (pstate->p_expr_kind == EXPR_KIND_WHERE)
            ereport(ERROR,
                    (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                     errmsg("variable %s does not exist", rel->name),
                     parser_errposition(pstate, rel->location)));
    }

    if (!rel->name)
        rel->name = get_next_default_alias(cpstate);

    schema_name = get_graph_namespace_name(cpstate->graph_name);
    rel_name = get_label_relation_name(rel->label, cpstate->graph_oid);
    label_range_var = makeRangeVar(schema_name, rel_name, -1);
    alias = makeAlias(rel->name, NIL);

    rte = addRangeTableEntry(pstate, label_range_var, alias,
                             label_range_var->inh, true);
    /*
     * relation is visible (r.a in expression works) but attributes in the
     * relation are not visible (a in expression doesn't work)
     */
    addRTEtoQuery(pstate, rte, true, true, false);

    resno = pstate->p_next_resno++;

    expr = (Expr *)make_edge_expr(cpstate, rte, rel->label);

    if (rel->name)
    {
        te = makeTargetEntry(expr, resno, rel->name, false);
        *target_list = lappend(*target_list, te);
    }

    return expr;
}

static Expr *transform_cypher_node(cypher_parsestate *cpstate,
                                   cypher_node *node, List **target_list,
                                   bool output_node)
{
    ParseState *pstate = (ParseState *)cpstate;
    char *schema_name;
    char *rel_name;
    RangeVar *label_range_var;
    Alias *alias;
    RangeTblEntry *rte;
    int resno;
    TargetEntry *te;
    Expr *expr;

    if (!node->label)
        node->label = AG_DEFAULT_LABEL_VERTEX;
    else
    {
        /*
         *  XXX: Need to determine proper rules, for when label does not exist
         *  or is for an edge. Maybe labels and edges should share names, like
         *  in openCypher. But these are stand in errors, to prevent segmentation
         *  faults, and other errors.
         */
        label_cache_data *lcd =
            search_label_name_graph_cache(node->label, cpstate->graph_oid);

        if (lcd == NULL)
            ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                            errmsg("label %s does not exists", node->label),
                            parser_errposition(pstate, node->location)));

        if (lcd->kind != LABEL_KIND_VERTEX)
            ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                            errmsg("label %s is for edges, not vertices",
                                   node->label),
                            parser_errposition(pstate, node->location)));
    }

    if (!output_node)
        return NULL;

    if (node->name != NULL)
    {
        TargetEntry *te = findTarget(*target_list, node->name);
        /* also search for the variable from a previous transforms */
        Node *expr = colNameToVar(pstate, node->name, false, node->location);

        if (expr != NULL)
            return (Expr*)expr;

        if (te != NULL)
        {
            transform_entity *entity = find_variable(cpstate, node->name);
            /*
             * TODO: openCypher allows a variable to be used before it
             * is properly declared. This logic is not satifactory
             * for that and must be better developed.
             */
            if (entity != NULL &&
                (entity->type != ENT_VERTEX ||
                 !IS_DEFAULT_LABEL_VERTEX(node->label) ||
                 node->props))
                ereport(ERROR,
                        (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                         errmsg("variable %s already exists", node->name),
                         parser_errposition(pstate, node->location)));

            return te->expr;
        }

        /*
         * If we are in a WHERE clause transform, we don't want to create new
         * variables, we want to use the existing ones. So, error if otherwise.
         */
        if (pstate->p_expr_kind == EXPR_KIND_WHERE)
            ereport(ERROR,
                    (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                     errmsg("variable `%s` does not exist", node->name),
                     parser_errposition(pstate, node->location)));
    }

    if (!node->name)
        node->name = get_next_default_alias(cpstate);

    schema_name = get_graph_namespace_name(cpstate->graph_name);
    rel_name = get_label_relation_name(node->label, cpstate->graph_oid);
    label_range_var = makeRangeVar(schema_name, rel_name, -1);
    alias = makeAlias(node->name, NIL);

    rte = addRangeTableEntry(pstate, label_range_var, alias,
                             label_range_var->inh, true);
    /*
     * relation is visible (r.a in expression works) but attributes in the
     * relation are not visible (a in expression doesn't work)
     */
    addRTEtoQuery(pstate, rte, true, true, true);

    resno = pstate->p_next_resno++;

    expr = (Expr *)make_vertex_expr(cpstate, rte, node->label);

    if (node->name)
    {
        te = makeTargetEntry(expr, resno, node->name, false);
        *target_list = lappend(*target_list, te);
    }

    return expr;
}

static Node *make_edge_expr(cypher_parsestate *cpstate, RangeTblEntry *rte,
                            char *label)
{
    ParseState *pstate = (ParseState *)cpstate;
    Oid label_name_func_oid;
    Oid func_oid;
    Node *id, *start_id, *end_id;
    Const *graph_oid_const;
    Node *props;
    List *args, *label_name_args;
    FuncExpr *func_expr;
    FuncExpr *label_name_func_expr;

    func_oid = get_ag_func_oid("_agtype_build_edge", 5, GRAPHIDOID, GRAPHIDOID,
                               GRAPHIDOID, CSTRINGOID, AGTYPEOID);

    id = scanRTEForColumn(pstate, rte, AG_EDGE_COLNAME_ID, -1, 0, NULL);

    start_id = scanRTEForColumn(pstate, rte, AG_EDGE_COLNAME_START_ID, -1, 0,
                                NULL);

    end_id = scanRTEForColumn(pstate, rte, AG_EDGE_COLNAME_END_ID, -1, 0,
                              NULL);

    label_name_func_oid = get_ag_func_oid("_label_name", 2, OIDOID,
                                          GRAPHIDOID);

    graph_oid_const = makeConst(OIDOID, -1, InvalidOid, sizeof(Oid),
                                ObjectIdGetDatum(cpstate->graph_oid), false,
                                true);

    label_name_args = list_make2(graph_oid_const, id);

    label_name_func_expr = makeFuncExpr(label_name_func_oid, CSTRINGOID,
                                        label_name_args, InvalidOid,
                                        InvalidOid, COERCE_EXPLICIT_CALL);
    label_name_func_expr->location = -1;

    props = scanRTEForColumn(pstate, rte, AG_EDGE_COLNAME_PROPERTIES, -1, 0,
                             NULL);

    args = list_make5(id, start_id, end_id, label_name_func_expr, props);

    func_expr = makeFuncExpr(func_oid, AGTYPEOID, args, InvalidOid, InvalidOid,
                             COERCE_EXPLICIT_CALL);
    func_expr->location = -1;

    return (Node *)func_expr;
}
static Node *make_vertex_expr(cypher_parsestate *cpstate, RangeTblEntry *rte,
                              char *label)
{
    ParseState *pstate = (ParseState *)cpstate;
    Oid label_name_func_oid;
    Oid func_oid;
    Node *id;
    Const *graph_oid_const;
    Node *props;
    List *args, *label_name_args;
    FuncExpr *func_expr;
    FuncExpr *label_name_func_expr;

    func_oid = get_ag_func_oid("_agtype_build_vertex", 3, GRAPHIDOID,
                               CSTRINGOID, AGTYPEOID);

    id = scanRTEForColumn(pstate, rte, AG_VERTEX_COLNAME_ID, -1, 0, NULL);

    label_name_func_oid = get_ag_func_oid("_label_name", 2, OIDOID,
                                          GRAPHIDOID);

    graph_oid_const = makeConst(OIDOID, -1, InvalidOid, sizeof(Oid),
                                ObjectIdGetDatum(cpstate->graph_oid), false,
                                true);

    label_name_args = list_make2(graph_oid_const, id);

    label_name_func_expr = makeFuncExpr(label_name_func_oid, CSTRINGOID,
                                        label_name_args, InvalidOid,
                                        InvalidOid, COERCE_EXPLICIT_CALL);
    label_name_func_expr->location = -1;

    props = scanRTEForColumn(pstate, rte, AG_VERTEX_COLNAME_PROPERTIES, -1, 0,
                             NULL);

    args = list_make3(id, label_name_func_expr, props);

    func_expr = makeFuncExpr(func_oid, AGTYPEOID, args, InvalidOid, InvalidOid,
                             COERCE_EXPLICIT_CALL);
    func_expr->location = -1;

    return (Node *)func_expr;
}

static Query *transform_cypher_create(cypher_parsestate *cpstate,
                                      cypher_clause *clause)
{
    ParseState *pstate = (ParseState *)cpstate;
    cypher_create *self = (cypher_create *)clause->self;
    cypher_create_target_nodes *target_nodes;
    Const *pattern_const;
    Const *null_const;
    List *transformed_pattern;
    Expr *func_expr;
    Oid func_create_oid;
    Query *query;
    TargetEntry *tle;
    StringInfo str = makeStringInfo();

    target_nodes = make_ag_node(cypher_create_target_nodes);
    target_nodes->flags = CYPHER_CLAUSE_FLAG_NONE;
    target_nodes->graph_oid = cpstate->graph_oid;

    query = makeNode(Query);
    query->commandType = CMD_SELECT;
    query->targetList = NIL;

    if (clause->prev)
    {
        RangeTblEntry *rte;
        int rtindex;

        rte = transform_prev_cypher_clause(cpstate, clause->prev);
        rtindex = list_length(pstate->p_rtable);
        Assert(rtindex == 1); // rte is the first RangeTblEntry in pstate
        query->targetList = expandRelAttrs(pstate, rte, rtindex, 0, -1);

        target_nodes->flags |= CYPHER_CLAUSE_FLAG_PREVIOUS_CLAUSE;
    }

    func_create_oid = get_ag_func_oid(CREATE_CLAUSE_FUNCTION_NAME, 1, INTERNALOID);

    null_const = makeNullConst(AGTYPEOID, -1, InvalidOid);
    tle = makeTargetEntry((Expr *)null_const, pstate->p_next_resno++,
                          AGE_VARNAME_CREATE_NULL_VALUE, false);
    query->targetList = lappend(query->targetList, tle);

    /*
     * Create the Const Node to hold the pattern. skip the parse node,
     * because we would not be able to control how our pointer to the
     * internal type is copied.
     */
    transformed_pattern = transform_cypher_create_pattern(cpstate, query,
                                                          self->pattern);

    target_nodes->paths = transformed_pattern;
    if (!clause->next)
    {
        target_nodes->flags |= CYPHER_CLAUSE_FLAG_TERMINAL;
    }

    /*
     * Serialize the cypher_create_target_nodes data structure. In certain
     * cases (Prepared Statements and PL/pgsql), the MemoryContext that
     * it is stored in will be destroyed. We need to get it into a format
     * that Postgres' can copy between MemoryContexts. Just making it into
     * an ExtensibleNode does not work, because there are certain parts of
     * Postgres that cannot handle an ExtensibleNode in a function call.
     * So we serialize the data structure and place it into a Const node
     * that can handle these situations AND be copied correctly.
     */
    outNode(str, target_nodes);

    pattern_const = makeConst(INTERNALOID, -1, InvalidOid, str->len,
                              PointerGetDatum(str->data), false, false);

    /*
     * Create the FuncExpr Node.
     * NOTE: We can't use Postgres' transformExpr function, because it will
     * recursively transform the arguments, and our internal type would
     * force an error to be thrown.
     */
    func_expr = (Expr *)makeFuncExpr(func_create_oid, AGTYPEOID,
                                     list_make1(pattern_const), InvalidOid,
                                     InvalidOid, COERCE_EXPLICIT_CALL);

    // Create the target entry
    tle = makeTargetEntry(func_expr, pstate->p_next_resno++,
                          AGE_VARNAME_CREATE_CLAUSE, false);
    query->targetList = lappend(query->targetList, tle);

    query->rtable = pstate->p_rtable;
    query->jointree = makeFromExpr(pstate->p_joinlist, NULL);

    return query;
}

static List *transform_cypher_create_pattern(cypher_parsestate *cpstate,
                                             Query *query, List *pattern)
{
    ListCell *lc;
    List *transformed_pattern = NIL;

    foreach (lc, pattern)
    {
        cypher_create_path *transformed_path;

        transformed_path = transform_cypher_create_path(
            cpstate, &query->targetList, lfirst(lc));

        transformed_pattern = lappend(transformed_pattern, transformed_path);
    }

    return transformed_pattern;
}

static cypher_create_path *
transform_cypher_create_path(cypher_parsestate *cpstate, List **target_list,
                             cypher_path *path)
{
    ParseState *pstate = (ParseState *)cpstate;
    ListCell *lc;
    List *transformed_path = NIL;
    cypher_create_path *ccp = make_ag_node(cypher_create_path);
    bool in_path = path->var_name != NULL;

    ccp->path_attr_num = InvalidAttrNumber;

    foreach (lc, path->path)
    {
        if (is_ag_node(lfirst(lc), cypher_node))
        {
            cypher_node *node = lfirst(lc);
            transform_entity *entity;

            cypher_target_node *rel =
                transform_create_cypher_node(cpstate, target_list, node);

            if (in_path)
                rel->flags |= CYPHER_TARGET_NODE_IN_PATH_VAR;

            transformed_path = lappend(transformed_path, rel);

            entity = make_transform_entity(cpstate, ENT_VERTEX, (Node *)node,
                                           NULL);

            cpstate->entities = lappend(cpstate->entities, entity);
        }
        else if (is_ag_node(lfirst(lc), cypher_relationship))
        {
            cypher_relationship *edge = lfirst(lc);
            transform_entity *entity;

            cypher_target_node *rel =
                transform_create_cypher_edge(cpstate, target_list, edge);

            if (in_path)
                rel->flags |= CYPHER_TARGET_NODE_IN_PATH_VAR;

            transformed_path = lappend(transformed_path, rel);

            entity = make_transform_entity(cpstate, ENT_EDGE, (Node *)edge,
                                           NULL);

            cpstate->entities = lappend(cpstate->entities, entity);
        }
        else
        {
            ereport(ERROR,
                    (errmsg_internal("unreconized node in create pattern")));
        }
    }

    ccp->target_nodes = transformed_path;

    /*
     * If this path a variable, create a placeholder entry that we can fill
     * in with during the execution phase.
     */
    if (path->var_name)
    {
        TargetEntry *te;

        if (list_length(transformed_path) < 3)
        {
            ereport(
                ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("paths consist of alternating vertices and edges."),
                 parser_errposition(pstate, path->location),
                 errhint("paths require at least 2 vertices and 1 edge")));
        }

        te = placeholder_target_entry(cpstate, path->var_name);

        ccp->path_attr_num = te->resno;

        *target_list = lappend(*target_list, te);
    }

    return ccp;
}

static cypher_target_node *
transform_create_cypher_edge(cypher_parsestate *cpstate, List **target_list,
                             cypher_relationship *edge)
{
    ParseState *pstate = (ParseState *)cpstate;
    cypher_target_node *rel = make_ag_node(cypher_target_node);
    Expr *props;
    Relation label_relation;
    RangeVar *rv;
    RangeTblEntry *rte;
    TargetEntry *te;
    char *alias;
    AttrNumber resno;

    rel->type = LABEL_KIND_EDGE;
    rel->flags = CYPHER_TARGET_NODE_FLAG_INSERT;
    rel->label_name = edge->label;
    rel->resultRelInfo = NULL;

    if (edge->name)
    {
        /*
         * Variables can be declared in a CREATE clause, but not used if
         * it already exists.
         */
        if (variable_exists(cpstate, edge->name))
            ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                            errmsg("variable %s already exists", edge->name)));

        rel->variable_name = edge->name;
        te = placeholder_target_entry(cpstate, edge->name);
        rel->tuple_position = te->resno;
        *target_list = lappend(*target_list, te);

        rel->flags |= CYPHER_TARGET_NODE_IS_VAR;
    }
    else
    {
        rel->variable_name = NULL;
        rel->tuple_position = 0;
    }

    if (edge->dir == CYPHER_REL_DIR_NONE)
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("only directed relationships are allowed in CREATE"),
                 parser_errposition(&cpstate->pstate, edge->location)));

    rel->dir = edge->dir;

    if (!edge->label)
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("relationships must be specify a label in CREATE."),
                 parser_errposition(&cpstate->pstate, edge->location)));

    // create the label entry if it does not exist
    if (!label_exists(edge->label, cpstate->graph_oid))
    {
        List *parent;
        RangeVar *rv;

        rv = get_label_range_var(cpstate->graph_name, cpstate->graph_oid,
                                 AG_DEFAULT_LABEL_EDGE);

        parent = list_make1(rv);

        create_label(cpstate->graph_name, edge->label, LABEL_TYPE_EDGE,
                     parent);
    }

    // lock the relation of the label
    rv = makeRangeVar(cpstate->graph_name, edge->label, -1);
    label_relation = parserOpenTable(&cpstate->pstate, rv, RowExclusiveLock);

    // Store the relid
    rel->relid = RelationGetRelid(label_relation);

    rte = addRangeTableEntryForRelation((ParseState *)cpstate, label_relation,
                                        NULL, false, false);
    rte->requiredPerms = ACL_INSERT;

    // Build Id expression, always use the default logic
    rel->id_expr = (Expr *)build_column_default(label_relation,
                                      Anum_ag_label_edge_table_id);

    // Build properties expression, if no map is given, use the default logic
    alias = get_next_default_alias(cpstate);
    resno = pstate->p_next_resno++;

    props = cypher_create_properties(cpstate, rel, label_relation, edge->props,
                                     ENT_EDGE);

    rel->prop_attr_num = resno - 1;
    te = makeTargetEntry(props, resno, alias, false);

    *target_list = lappend(*target_list, te);

    // Keep the lock
    heap_close(label_relation, NoLock);

    return rel;
}

static bool variable_exists(cypher_parsestate *cpstate, char *name)
{
    ParseState *pstate = (ParseState *)cpstate;
    Node *id;
    RangeTblEntry *rte;

    if (name == NULL)
        return false;

    rte = find_rte(cpstate, PREV_CYPHER_CLAUSE_ALIAS);
    if (rte)
    {
        id = scanRTEForColumn(pstate, rte, name, -1, 0, NULL);

        return id != NULL;
    }

    return false;
}

// transform nodes, check to see if the variable name already exists.
static cypher_target_node *
transform_create_cypher_node(cypher_parsestate *cpstate, List **target_list,
                             cypher_node *node)
{
    ParseState *pstate = (ParseState *)cpstate;

    /*
     *  Check if the variable already exists, if so find the entity and
     *  setup the target node
     */
    if (node->name)
    {
        transform_entity *entity;

        entity = find_variable(cpstate, node->name);

        if (entity)
        {
            if (entity->type != ENT_VERTEX)
                ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                                errmsg("variable %s already exists", node->name),
                                parser_errposition(pstate, node->location)));

            return transform_create_cypher_existing_node(cpstate, target_list,
                                                         entity->declared_in_current_clause, node);
        }
    }

    // otherwise transform the target node as a new node
    return transform_create_cypher_new_node(cpstate, target_list, node);
}

/*
 * Returns the resno for the TargetEntry with the resname equal to the name
 * passed. Returns -1 otherwise.
 */
static int get_target_entry_resno(List *target_list, char *name)
{
    ListCell *lc;

    foreach (lc, target_list)
    {
        TargetEntry *te = (TargetEntry *)lfirst(lc);
        if (!strcmp(te->resname, name))
        {
            te->expr = add_volatile_wrapper(te->expr);
            return te->resno;
        }
    }

    return -1;
}

/*
 * Transform logic for a previously declared variable in a CREATE clause.
 * All we need from the variable node is its id, and whether we can skip
 * some tests in the execution phase..
 */
static cypher_target_node *transform_create_cypher_existing_node(
    cypher_parsestate *cpstate, List **target_list, bool declared_in_current_clause,
    cypher_node *node)
{
    cypher_target_node *rel = make_ag_node(cypher_target_node);

    rel->type = LABEL_KIND_VERTEX;
    rel->flags = CYPHER_TARGET_NODE_FLAG_NONE;
    rel->resultRelInfo = NULL;
    rel->variable_name = node->name;


    if (node->props)
        ereport(
            ERROR,
            (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
             errmsg(
                 "previously declared nodes in a create clause cannot have properties")));

    if (node->label)
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("previously declared variables cannot have a label")));

    /*
     * When the variable is declared in the same clause this vertex is a part of
     * we can skip some expensive checks in the execution phase.
     */
    if (declared_in_current_clause)
    {
        rel->flags |= EXISTING_VARAIBLE_DECLARED_SAME_CLAUSE;
    }

    // get the AttrNumber the variable is stored in, so we can extract the id later.
    rel->tuple_position = get_target_entry_resno(*target_list, node->name);

    return rel;
}

/*
 * Transform logic for a node in a create clause that was not previously
 * declared.
 */
static cypher_target_node *
transform_create_cypher_new_node(cypher_parsestate *cpstate,
                                 List **target_list, cypher_node *node)
{
    ParseState *pstate = (ParseState *)cpstate;
    cypher_target_node *rel = make_ag_node(cypher_target_node);
    Relation label_relation;
    RangeVar *rv;
    RangeTblEntry *rte;
    TargetEntry *te;
    Expr *props;
    char *alias;
    int resno;

    rel->type = LABEL_KIND_VERTEX;
    rel->tuple_position = InvalidAttrNumber;
    rel->variable_name = NULL;
    rel->resultRelInfo = NULL;

    if (!node->label)
    {
        rel->label_name = "";
        /*
         *  If no label is specified, assign the generic label name that
         *  all labels are descendents of.
         */
        node->label = AG_DEFAULT_LABEL_VERTEX;
    }
    else
    {
        rel->label_name = node->label;
    }

    // create the label entry if it does not exist
    if (!label_exists(node->label, cpstate->graph_oid))
    {
        List *parent;
        RangeVar *rv;

        rv = get_label_range_var(cpstate->graph_name, cpstate->graph_oid,
                                 AG_DEFAULT_LABEL_VERTEX);

        parent = list_make1(rv);

        create_label(cpstate->graph_name, node->label, LABEL_TYPE_VERTEX,
                     parent);
    }

    rel->flags = CYPHER_TARGET_NODE_FLAG_INSERT;

    rv = makeRangeVar(cpstate->graph_name, node->label, -1);
    label_relation = parserOpenTable(&cpstate->pstate, rv, RowExclusiveLock);

    // Store the relid
    rel->relid = RelationGetRelid(label_relation);

    rte = addRangeTableEntryForRelation((ParseState *)cpstate, label_relation,
                                        NULL, false, false);
    rte->requiredPerms = ACL_INSERT;

    // id
    rel->id_expr = (Expr *)build_column_default(label_relation,
                                                Anum_ag_label_vertex_table_id);

    // properties
    alias = get_next_default_alias(cpstate);
    resno = pstate->p_next_resno++;

    props = cypher_create_properties(cpstate, rel, label_relation, node->props,
                                     ENT_VERTEX);

    rel->prop_attr_num = resno - 1;
    te = makeTargetEntry(props, resno, alias, false);
    *target_list = lappend(*target_list, te);

    heap_close(label_relation, NoLock);

    if (node->name)
    {
        rel->variable_name = node->name;
        te = placeholder_target_entry(cpstate, node->name);
        rel->tuple_position = te->resno;
        *target_list = lappend(*target_list, te);
        rel->flags |= CYPHER_TARGET_NODE_IS_VAR;
    }

    return rel;
}

/*
 * Variable Edges cannot be created until the executor phase, because we
 * don't know what their start and end node ids will be. Therefore, path
 * variables cannot be created either. Create a placeholder entry that we
 * will replace in the execution phase. Do this for nodes too, to be
 * consistent.
 */
static TargetEntry *placeholder_target_entry(cypher_parsestate *cpstate,
                                             char *name)
{
    ParseState *pstate = (ParseState *)cpstate;
    Expr *n;
    int resno;

    n = (Expr *)makeNullConst(AGTYPEOID, -1, InvalidOid);
    n = add_volatile_wrapper(n);

    resno = pstate->p_next_resno++;

    return makeTargetEntry(n, resno, name, false);
}

/*
 * Build the target list for an entity that is not a previously declared
 * variable.
 */
static Expr *cypher_create_properties(cypher_parsestate *cpstate,
                                      cypher_target_node *rel,
                                      Relation label_relation, Node *props,
                                      enum transform_entity_type type)
{
    Expr *properties;

    if (props != NULL && is_ag_node(props, cypher_param))
    {
        ParseState *pstate = (ParseState *) cpstate;
        cypher_param *param = (cypher_param *)props;

        ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                errmsg("properties in a CREATE clause as a parameter is not supported"),
                parser_errposition(pstate, param->location)));
    }

    if (props)
    {
        properties = (Expr *)transform_cypher_expr(cpstate, props,
                                                   EXPR_KIND_INSERT_TARGET);
    }
    else if (type == ENT_VERTEX)
    {
        properties = (Expr *)build_column_default(
            label_relation, Anum_ag_label_vertex_table_properties);
    }
    else if (type == ENT_EDGE)
    {
        properties = (Expr *)build_column_default(
            label_relation, Anum_ag_label_edge_table_properties);
    }
    else
    {
	ereport(ERROR, (errmsg_internal("unreconized entity type")));
    }

    // add a volatile wrapper call to prevent the optimizer from removing it
    return (Expr *)add_volatile_wrapper(properties);
}

/*
 * makeNamespaceItem (from PG makeNamespaceItem)-
 *        Convenience subroutine to construct a ParseNamespaceItem.
 */
static ParseNamespaceItem *makeNamespaceItem(RangeTblEntry *rte,
                                             bool rel_visible,
                                             bool cols_visible,
                                             bool lateral_only, bool lateral_ok)
{
    ParseNamespaceItem *nsitem;

    nsitem = (ParseNamespaceItem *) palloc(sizeof(ParseNamespaceItem));
    nsitem->p_rte = rte;
    nsitem->p_rel_visible = rel_visible;
    nsitem->p_cols_visible = cols_visible;
    nsitem->p_lateral_only = lateral_only;
    nsitem->p_lateral_ok = lateral_ok;
    return nsitem;
}

/*
 * This function is similar to transformFromClause() that is called with a
 * single RangeSubselect.
 */
static RangeTblEntry *
transform_cypher_clause_as_subquery(cypher_parsestate *cpstate,
                                    transform_method transform,
                                    cypher_clause *clause)
{
    ParseState *pstate = (ParseState *)cpstate;
    bool lateral = false;
    Query *query;
    RangeTblEntry *rte;
    Alias *alias;
    ParseExprKind old_expr_kind = pstate->p_expr_kind;

    /*
     * We allow expression kinds of none, where, and subselect. Others MAY need
     * to be added depending. However, at this time, only these are needed.
     */
    Assert(pstate->p_expr_kind == EXPR_KIND_NONE ||
           pstate->p_expr_kind == EXPR_KIND_WHERE ||
           pstate->p_expr_kind == EXPR_KIND_FROM_SUBSELECT);

    /*
     * As these are all sub queries, if this is just of type NONE, note it as a
     * SUBSELECT. Other types will be dealt with as needed.
     */
    if (pstate->p_expr_kind == EXPR_KIND_NONE)
        pstate->p_expr_kind = EXPR_KIND_FROM_SUBSELECT;
    /*
     * If this is a WHERE, pass it through and set lateral to true because it
     * needs to see what comes before it.
     */
    if (pstate->p_expr_kind == EXPR_KIND_WHERE)
        lateral = true;

    pstate->p_lateral_active = lateral;

    query = analyze_cypher_clause(transform, clause, cpstate);

    /* set pstate kind back */
    pstate->p_expr_kind = old_expr_kind;

    alias = makeAlias(PREV_CYPHER_CLAUSE_ALIAS, NIL);

    rte = addRangeTableEntryForSubquery(pstate, query, alias, lateral, true);

    /*
     * NOTE: skip namespace conflicts check if the rte will be the only
     *       RangeTblEntry in pstate
     */
    if (list_length(pstate->p_rtable) > 1)
    {
        List *namespace;
        int rtindex;

        rtindex = list_length(pstate->p_rtable);
        Assert(rte == rt_fetch(rtindex, pstate->p_rtable));

        namespace = list_make1(makeNamespaceItem(rte, true, true, false, true));

        checkNameSpaceConflicts(pstate, pstate->p_namespace, namespace);
    }

    // all variables(attributes) from the previous clause(subquery) are visible
    addRTEtoQuery(pstate, rte, true, false, true);

    /* set pstate lateral back */
    pstate->p_lateral_active = false;

    return rte;
}

/*
 * When we are done transforming a clause, before transforming the next clause
 * iterate through the transform entities and mark them as not belonging to
 * the clause that is currently being transformed.
 */
static void advance_transform_entities_to_next_clause(List *entities)
{
    ListCell *lc;

    foreach (lc, entities)
    {
        transform_entity *entity = lfirst(lc);

        entity->declared_in_current_clause = false;
    }
}

static Query *analyze_cypher_clause(transform_method transform,
                                    cypher_clause *clause,
                                    cypher_parsestate *parent_cpstate)
{
    cypher_parsestate *cpstate;
    Query *query;
    ParseState *parent_pstate = (ParseState*)parent_cpstate;
    ParseState *pstate;

    cpstate = make_cypher_parsestate(parent_cpstate);
    pstate = (ParseState*)cpstate;

    /* copy the expr_kind down to the child */
    pstate->p_expr_kind = parent_pstate->p_expr_kind;

    query = transform(cpstate, clause);

    advance_transform_entities_to_next_clause(cpstate->entities);

    parent_cpstate->entities = list_concat(parent_cpstate->entities,
                                           cpstate->entities);
    free_cypher_parsestate(cpstate);

    return query;
}

static TargetEntry *findTarget(List *targetList, char *resname)
{
    ListCell *lt;
    TargetEntry *te = NULL;

    if (resname == NULL)
        return NULL;

    foreach (lt, targetList)
    {
        te = lfirst(lt);

        if (te->resjunk)
            continue;

        if (strcmp(te->resname, resname) == 0)
            return te;
    }

    return NULL;
}

/*
 * Wrap the expression with a volatile function, to prevent the optimer from
 * elimating the expression.
 */
static Expr *add_volatile_wrapper(Expr *node)
{
    Oid oid;

    oid = get_ag_func_oid("agtype_volatile_wrapper", 1, AGTYPEOID);

    return (Expr *)makeFuncExpr(oid, AGTYPEOID, list_make1(node), InvalidOid,
                                InvalidOid, COERCE_EXPLICIT_CALL);
}

/*
 * from postgresql parse_sub_analyze
 * Entry point for recursively analyzing a sub-statement.
 */
Query *cypher_parse_sub_analyze(Node *parseTree,
                                cypher_parsestate *cpstate,
                                CommonTableExpr *parentCTE,
                                bool locked_from_parent,
                                bool resolve_unknowns)
{
    ParseState *pstate = make_parsestate((ParseState*)cpstate);
    cypher_clause *clause;
    Query *query;

    pstate->p_parent_cte = parentCTE;
    pstate->p_locked_from_parent = locked_from_parent;
    pstate->p_resolve_unknowns = resolve_unknowns;

    clause = palloc(sizeof(cypher_clause));
    clause->self = parseTree;
    query = transform_cypher_clause(cpstate, clause);

    free_parsestate(pstate);

    return query;
}
