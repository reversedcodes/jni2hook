#include "jni2hook/utils/visitors/constructor_init.h"

#include <stdlib.h>
#include <string.h>

enum
{
    TRACE_THIS = 1,
    TRACE_OTHER = 2
};

typedef struct
{
    bool reached;
    bool queued;
    u2 depth;
    u1 *locals;
    u1 *stack;
} trace_state;

typedef struct
{
    u1 *locals;
    u1 *stack;
    u2 depth;
    u2 max_locals;
    u2 max_stack;
} trace_work;

static bool pop_slots(trace_work *work, u2 count)
{
    if (count > work->depth)
        return false;
    work->depth = (u2)(work->depth - count);
    return true;
}

static bool push_slot(trace_work *work, u1 value)
{
    if (work->depth >= work->max_stack)
        return false;
    work->stack[work->depth++] = value;
    return true;
}

static bool push_other(trace_work *work, u2 count)
{
    for (u2 i = 0; i < count; i++)
    {
        if (!push_slot(work, TRACE_OTHER))
            return false;
    }
    return true;
}

static bool replace_slots(trace_work *work, u2 popped, u2 pushed)
{
    return pop_slots(work, popped) && push_other(work, pushed);
}

static bool descriptor_slots(const constant_pool *pool, u2 index,
                             u2 *arguments, u2 *result)
{
    const u1 *bytes = NULL;
    u2 length = 0;
    if (!constant_pool_utf8(pool, index, &bytes, &length) || length == 0)
        return false;

    u2 cursor = 0;
    u2 args = 0;
    if (bytes[cursor] == JVM_SIGNATURE_FUNC)
    {
        cursor++;
        while (cursor < length && bytes[cursor] != JVM_SIGNATURE_ENDFUNC)
        {
            while (cursor < length && bytes[cursor] == JVM_SIGNATURE_ARRAY)
                cursor++;
            if (cursor >= length)
                return false;
            if (bytes[cursor] == JVM_SIGNATURE_CLASS)
            {
                do
                {
                    cursor++;
                } while (cursor < length && bytes[cursor] != JVM_SIGNATURE_ENDCLASS);
                if (cursor >= length)
                    return false;
                cursor++;
                args++;
            }
            else
            {
                args = (u2)(args + (bytes[cursor] == JVM_SIGNATURE_LONG ||
                                    bytes[cursor] == JVM_SIGNATURE_DOUBLE ? 2u : 1u));
                cursor++;
            }
        }
        if (cursor >= length)
            return false;
        cursor++;
    }

    if (cursor >= length)
        return false;
    while (bytes[cursor] == JVM_SIGNATURE_ARRAY)
    {
        cursor++;
        if (cursor >= length)
            return false;
    }

    u2 returned = 1;
    if (bytes[cursor] == JVM_SIGNATURE_VOID)
        returned = 0;
    else if (bytes[cursor] == JVM_SIGNATURE_LONG || bytes[cursor] == JVM_SIGNATURE_DOUBLE)
        returned = 2;

    *arguments = args;
    *result = returned;
    return true;
}

static bool member_descriptor(const constant_pool *pool, u2 cp_index,
                              u2 *arguments, u2 *result)
{
    const cp_info *entry = constant_pool_at(pool, cp_index);
    if (entry == NULL)
        return false;

    u2 nat_index = 0;
    if (entry->tag == JVM_CONSTANT_Fieldref || entry->tag == JVM_CONSTANT_Methodref ||
        entry->tag == JVM_CONSTANT_InterfaceMethodref)
        nat_index = entry->u.ref.nat_index;
    else if (entry->tag == JVM_CONSTANT_InvokeDynamic || entry->tag == JVM_CONSTANT_Dynamic)
        nat_index = entry->u.dynamic.nat_index;
    else
        return false;

    const cp_info *nat = constant_pool_at(pool, nat_index);
    return nat != NULL && nat->tag == JVM_CONSTANT_NameAndType &&
           descriptor_slots(pool, nat->u.nat.descriptor_index, arguments, result);
}

static bool is_initializing_call(const ClassFile *cf, const instruction *node,
                                 u2 *arguments)
{
    if (node->opcode != JVM_OPC_invokespecial || node->kind != OPERAND_CP_INDEX)
        return false;

    const cp_info *methodref = constant_pool_at(&cf->constant_pool, node->u.cp.index);
    if (methodref == NULL || methodref->tag != JVM_CONSTANT_Methodref ||
        (methodref->u.ref.class_index != cf->this_class &&
         methodref->u.ref.class_index != cf->super_class))
        return false;

    const cp_info *nat = constant_pool_at(&cf->constant_pool, methodref->u.ref.nat_index);
    u2 ignored = 0;
    return nat != NULL && nat->tag == JVM_CONSTANT_NameAndType &&
           constant_pool_utf8_equals(&cf->constant_pool, nat->u.nat.name_index, "<init>") &&
           descriptor_slots(&cf->constant_pool, nat->u.nat.descriptor_index,
                            arguments, &ignored);
}

static bool load_local(trace_work *work, u2 index, u2 width, bool reference)
{
    if (index >= work->max_locals || width > (u2)(work->max_locals - index))
        return false;
    if (reference && !push_slot(work, work->locals[index]))
        return false;
    return reference || push_other(work, width);
}

static bool store_local(trace_work *work, u2 index, u2 width, bool reference)
{
    if (index >= work->max_locals || width > (u2)(work->max_locals - index) ||
        width > work->depth)
        return false;
    const u1 value = reference ? work->stack[work->depth - 1u] : TRACE_OTHER;
    if (!pop_slots(work, width))
        return false;
    for (u2 i = 0; i < width; i++)
        work->locals[index + i] = value;
    return true;
}

static bool duplicate_slots(trace_work *work, u2 copied, u2 below)
{
    if ((u4)copied + below > work->depth ||
        (u4)work->depth + copied > work->max_stack)
        return false;
    const u2 source = (u2)(work->depth - copied);
    const u2 insertion = (u2)(source - below);
    memmove(work->stack + insertion + copied, work->stack + insertion,
            (size_t)(work->depth - insertion) * sizeof(*work->stack));
    memcpy(work->stack + insertion, work->stack + source + copied,
           (size_t)copied * sizeof(*work->stack));
    work->depth = (u2)(work->depth + copied);
    return true;
}

static bool field_width(const constant_pool *pool, u2 cp_index, u2 *width)
{
    u2 ignored = 0;
    return member_descriptor(pool, cp_index, &ignored, width);
}

static bool execute(const ClassFile *cf, const instruction *node, trace_work *work,
                    bool *terminal)
{
    *terminal = false;
    const u1 opcode = node->opcode;

    if (opcode == JVM_OPC_nop || opcode == JVM_OPC_iinc)
        return true;
    if ((opcode >= JVM_OPC_aconst_null && opcode <= JVM_OPC_iconst_5) ||
        (opcode >= JVM_OPC_fconst_0 && opcode <= JVM_OPC_fconst_2) ||
        opcode == JVM_OPC_bipush || opcode == JVM_OPC_sipush ||
        opcode == JVM_OPC_ldc || opcode == JVM_OPC_ldc_w)
        return push_other(work, 1);
    if (opcode == JVM_OPC_lconst_0 || opcode == JVM_OPC_lconst_1 ||
        opcode == JVM_OPC_dconst_0 || opcode == JVM_OPC_dconst_1 ||
        opcode == JVM_OPC_ldc2_w)
        return push_other(work, 2);

    if (opcode >= JVM_OPC_iload && opcode <= JVM_OPC_aload)
    {
        const u2 width = opcode == JVM_OPC_lload || opcode == JVM_OPC_dload ? 2u : 1u;
        return load_local(work, node->u.local.index, width, opcode == JVM_OPC_aload);
    }
    if (opcode >= JVM_OPC_iload_0 && opcode <= JVM_OPC_aload_3)
    {
        u2 index = 0;
        u2 width = 1;
        bool reference = false;
        if (opcode <= JVM_OPC_iload_3)
            index = (u2)(opcode - JVM_OPC_iload_0);
        else if (opcode <= JVM_OPC_lload_3)
        {
            index = (u2)(opcode - JVM_OPC_lload_0);
            width = 2;
        }
        else if (opcode <= JVM_OPC_fload_3)
            index = (u2)(opcode - JVM_OPC_fload_0);
        else if (opcode <= JVM_OPC_dload_3)
        {
            index = (u2)(opcode - JVM_OPC_dload_0);
            width = 2;
        }
        else
        {
            index = (u2)(opcode - JVM_OPC_aload_0);
            reference = true;
        }
        return load_local(work, index, width, reference);
    }

    if (opcode >= JVM_OPC_istore && opcode <= JVM_OPC_astore)
    {
        const u2 width = opcode == JVM_OPC_lstore || opcode == JVM_OPC_dstore ? 2u : 1u;
        return store_local(work, node->u.local.index, width, opcode == JVM_OPC_astore);
    }
    if (opcode >= JVM_OPC_istore_0 && opcode <= JVM_OPC_astore_3)
    {
        u2 index = 0;
        u2 width = 1;
        bool reference = false;
        if (opcode <= JVM_OPC_istore_3)
            index = (u2)(opcode - JVM_OPC_istore_0);
        else if (opcode <= JVM_OPC_lstore_3)
        {
            index = (u2)(opcode - JVM_OPC_lstore_0);
            width = 2;
        }
        else if (opcode <= JVM_OPC_fstore_3)
            index = (u2)(opcode - JVM_OPC_fstore_0);
        else if (opcode <= JVM_OPC_dstore_3)
        {
            index = (u2)(opcode - JVM_OPC_dstore_0);
            width = 2;
        }
        else
        {
            index = (u2)(opcode - JVM_OPC_astore_0);
            reference = true;
        }
        return store_local(work, index, width, reference);
    }

    if (opcode == JVM_OPC_laload || opcode == JVM_OPC_daload)
        return replace_slots(work, 2, 2);
    if (opcode >= JVM_OPC_iaload && opcode <= JVM_OPC_saload)
        return replace_slots(work, 2, 1);
    if (opcode == JVM_OPC_lastore || opcode == JVM_OPC_dastore)
        return pop_slots(work, 4);
    if (opcode >= JVM_OPC_iastore && opcode <= JVM_OPC_sastore)
        return pop_slots(work, 3);

    if (opcode == JVM_OPC_pop)
        return pop_slots(work, 1);
    if (opcode == JVM_OPC_pop2)
        return pop_slots(work, 2);
    if (opcode == JVM_OPC_dup)
        return duplicate_slots(work, 1, 0);
    if (opcode == JVM_OPC_dup_x1)
        return duplicate_slots(work, 1, 1);
    if (opcode == JVM_OPC_dup_x2)
        return duplicate_slots(work, 1, 2);
    if (opcode == JVM_OPC_dup2)
        return duplicate_slots(work, 2, 0);
    if (opcode == JVM_OPC_dup2_x1)
        return duplicate_slots(work, 2, 1);
    if (opcode == JVM_OPC_dup2_x2)
        return duplicate_slots(work, 2, 2);
    if (opcode == JVM_OPC_swap)
    {
        if (work->depth < 2)
            return false;
        const u1 top = work->stack[work->depth - 1u];
        work->stack[work->depth - 1u] = work->stack[work->depth - 2u];
        work->stack[work->depth - 2u] = top;
        return true;
    }

    if ((opcode >= JVM_OPC_iadd && opcode <= JVM_OPC_drem))
    {
        const u1 kind = (u1)((opcode - JVM_OPC_iadd) % 4u);
        return replace_slots(work, kind == 1u || kind == 3u ? 4u : 2u,
                             kind == 1u || kind == 3u ? 2u : 1u);
    }
    if (opcode >= JVM_OPC_ineg && opcode <= JVM_OPC_dneg)
        return true;
    if (opcode == JVM_OPC_ishl || opcode == JVM_OPC_ishr || opcode == JVM_OPC_iushr)
        return replace_slots(work, 2, 1);
    if (opcode == JVM_OPC_lshl || opcode == JVM_OPC_lshr || opcode == JVM_OPC_lushr)
        return replace_slots(work, 3, 2);
    if (opcode >= JVM_OPC_iand && opcode <= JVM_OPC_lxor)
        return replace_slots(work, (opcode & 1u) != 0u ? 4u : 2u,
                             (opcode & 1u) != 0u ? 2u : 1u);

    if (opcode == JVM_OPC_i2l || opcode == JVM_OPC_i2d ||
        opcode == JVM_OPC_f2l || opcode == JVM_OPC_f2d)
        return replace_slots(work, 1, 2);
    if (opcode == JVM_OPC_l2i || opcode == JVM_OPC_l2f ||
        opcode == JVM_OPC_d2i || opcode == JVM_OPC_d2f)
        return replace_slots(work, 2, 1);
    if (opcode == JVM_OPC_l2d || opcode == JVM_OPC_d2l)
        return replace_slots(work, 2, 2);
    if ((opcode >= JVM_OPC_i2f && opcode <= JVM_OPC_i2s) || opcode == JVM_OPC_f2i)
        return true;
    if (opcode == JVM_OPC_lcmp || opcode == JVM_OPC_dcmpl || opcode == JVM_OPC_dcmpg)
        return replace_slots(work, 4, 1);
    if (opcode == JVM_OPC_fcmpl || opcode == JVM_OPC_fcmpg)
        return replace_slots(work, 2, 1);

    if (opcode >= JVM_OPC_ifeq && opcode <= JVM_OPC_ifle)
        return pop_slots(work, 1);
    if (opcode >= JVM_OPC_if_icmpeq && opcode <= JVM_OPC_if_acmpne)
        return pop_slots(work, 2);
    if (opcode == JVM_OPC_tableswitch || opcode == JVM_OPC_lookupswitch ||
        opcode == JVM_OPC_ifnull || opcode == JVM_OPC_ifnonnull)
        return pop_slots(work, 1);
    if (opcode == JVM_OPC_goto || opcode == JVM_OPC_goto_w)
        return true;
    if (opcode == JVM_OPC_jsr || opcode == JVM_OPC_jsr_w || opcode == JVM_OPC_ret)
        return false;

    if (opcode >= JVM_OPC_ireturn && opcode <= JVM_OPC_return)
    {
        const u2 width = (u2)(opcode == JVM_OPC_lreturn || opcode == JVM_OPC_dreturn ? 2 :
                              opcode == JVM_OPC_return ? 0 : 1);
        *terminal = true;
        return pop_slots(work, width);
    }

    if (opcode >= JVM_OPC_getstatic && opcode <= JVM_OPC_putfield)
    {
        u2 width = 0;
        if (!field_width(&cf->constant_pool, node->u.cp.index, &width))
            return false;
        if (opcode == JVM_OPC_getstatic)
            return push_other(work, width);
        if (opcode == JVM_OPC_putstatic)
            return pop_slots(work, width);
        if (opcode == JVM_OPC_getfield)
            return replace_slots(work, 1, width);
        return pop_slots(work, (u2)(width + 1u));
    }

    if (opcode >= JVM_OPC_invokevirtual && opcode <= JVM_OPC_invokedynamic)
    {
        u2 cp_index = node->u.cp.index;
        if (node->kind == OPERAND_INVOKE_INTERFACE)
            cp_index = node->u.invoke_interface.index;
        else if (node->kind == OPERAND_INVOKE_DYNAMIC)
            cp_index = node->u.invoke_dynamic.index;
        u2 arguments = 0;
        u2 result = 0;
        if (!member_descriptor(&cf->constant_pool, cp_index, &arguments, &result))
            return false;
        const u2 receiver = (u2)(opcode == JVM_OPC_invokestatic ||
                                 opcode == JVM_OPC_invokedynamic ? 0 : 1);
        return replace_slots(work, (u2)(arguments + receiver), result);
    }

    if (opcode == JVM_OPC_new)
        return push_other(work, 1);
    if (opcode == JVM_OPC_newarray || opcode == JVM_OPC_anewarray)
        return replace_slots(work, 1, 1);
    if (opcode == JVM_OPC_multianewarray)
        return replace_slots(work, node->u.multianewarray.dimensions, 1);
    if (opcode == JVM_OPC_arraylength || opcode == JVM_OPC_instanceof)
        return replace_slots(work, 1, 1);
    if (opcode == JVM_OPC_athrow)
    {
        *terminal = true;
        return pop_slots(work, 1);
    }
    if (opcode == JVM_OPC_checkcast)
        return work->depth != 0;
    if (opcode == JVM_OPC_monitorenter || opcode == JVM_OPC_monitorexit)
        return pop_slots(work, 1);

    return false;
}

static bool instruction_index_at(const instruction_list *list, i4 offset, u4 *out)
{
    if (offset < 0)
        return false;
    const instruction *node = instruction_list_at_offset(list, (u4)offset);
    if (node == NULL)
        return false;
    *out = (u4)(node - list->items);
    return true;
}

static bool merge_state(trace_state *state, const trace_work *work,
                        u2 max_locals, u2 max_stack)
{
    if (!state->reached)
    {
        state->locals = malloc(max_locals == 0 ? 1u : max_locals);
        state->stack = malloc(max_stack == 0 ? 1u : max_stack);
        if (state->locals == NULL || state->stack == NULL)
            return false;
        memcpy(state->locals, work->locals, max_locals);
        memcpy(state->stack, work->stack, work->depth);
        state->depth = work->depth;
        state->reached = true;
        return true;
    }
    if (state->depth != work->depth)
        return false;

    bool changed = false;
    for (u2 i = 0; i < max_locals; i++)
    {
        const u1 merged = (u1)(state->locals[i] | work->locals[i]);
        changed = changed || merged != state->locals[i];
        state->locals[i] = merged;
    }
    for (u2 i = 0; i < work->depth; i++)
    {
        const u1 merged = (u1)(state->stack[i] | work->stack[i]);
        changed = changed || merged != state->stack[i];
        state->stack[i] = merged;
    }
    return changed;
}

static void free_states(trace_state *states, u4 count)
{
    if (states == NULL)
        return;
    for (u4 i = 0; i < count; i++)
    {
        free(states[i].locals);
        free(states[i].stack);
    }
    free(states);
}

static void propagate(trace_state *states, u4 target, const trace_work *work,
                      u2 max_locals, u2 max_stack, u4 *queue, u4 queue_size,
                      u4 *tail, u4 *pending)
{
    trace_state *next = &states[target];
    const bool changed = merge_state(next, work, max_locals, max_stack);
    if (changed && !next->queued)
    {
        queue[*tail] = target;
        *tail = (*tail + 1u) % queue_size;
        (*pending)++;
        next->queued = true;
    }
}

constructor_init_result constructor_init_offset(const ClassFile *cf, const code_editor *editor,
                                                u4 *out_offset)
{
    if (cf == NULL || editor == NULL || out_offset == NULL ||
        editor->instructions.count == 0 || editor->max_locals == 0)
        return CONSTRUCTOR_INIT_MISSING;

    const u4 count = editor->instructions.count;
    trace_state *states = calloc(count, sizeof(*states));
    u4 *queue = malloc((size_t)count * sizeof(*queue));
    u1 *locals = malloc(editor->max_locals);
    u1 *stack = malloc(editor->max_stack == 0 ? 1u : editor->max_stack);
    if (states == NULL || queue == NULL || locals == NULL || stack == NULL)
    {
        free_states(states, count);
        free(queue);
        free(locals);
        free(stack);
        return CONSTRUCTOR_INIT_MISSING;
    }

    memset(locals, TRACE_OTHER, editor->max_locals);
    locals[0] = TRACE_THIS;
    trace_work initial = {locals, stack, 0, editor->max_locals, editor->max_stack};
    if (!merge_state(&states[0], &initial, editor->max_locals, editor->max_stack))
    {
        free_states(states, count);
        free(queue);
        free(locals);
        free(stack);
        return CONSTRUCTOR_INIT_MISSING;
    }
    queue[0] = 0;
    states[0].queued = true;
    u4 head = 0;
    u4 tail = 1u % count;
    u4 pending = 1;
    bool found = false;
    bool ambiguous = false;
    u4 found_offset = 0;

    while (pending != 0)
    {
        const u4 index = queue[head];
        head = (head + 1u) % count;
        pending--;
        trace_state *state = &states[index];
        state->queued = false;
        memcpy(locals, state->locals, editor->max_locals);
        memcpy(stack, state->stack, state->depth);
        trace_work work = {locals, stack, state->depth,
                           editor->max_locals, editor->max_stack};
        const instruction *node = &editor->instructions.items[index];

        u2 arguments = 0;
        if (is_initializing_call(cf, node, &arguments) &&
            work.depth > arguments && work.stack[work.depth - arguments - 1u] == TRACE_THIS)
        {
            const u4 after = node->offset + instruction_length_at(node, node->offset);
            if (!found)
            {
                found = true;
                found_offset = after;
            }
            else if (after != found_offset)
            {
                /* A second, different initialising call means the paths do not
                   share one point after which this is initialised. Reported
                   rather than silently instrumenting whichever came first. */
                ambiguous = true;
            }
            continue;
        }

        bool terminal = false;
        if (!execute(cf, node, &work, &terminal))
            continue;

        if (node->kind == OPERAND_BRANCH || node->kind == OPERAND_BRANCH_WIDE)
        {
            u4 target = 0;
            if (instruction_index_at(&editor->instructions, node->u.branch.target, &target))
                propagate(states, target, &work, editor->max_locals, editor->max_stack,
                          queue, count, &tail, &pending);
            if (node->opcode != JVM_OPC_goto && node->opcode != JVM_OPC_goto_w &&
                node->opcode != JVM_OPC_jsr && node->opcode != JVM_OPC_jsr_w &&
                index + 1u < count)
                propagate(states, index + 1u, &work, editor->max_locals,
                          editor->max_stack, queue, count, &tail, &pending);
        }
        else if (node->kind == OPERAND_TABLE_SWITCH)
        {
            u4 target = 0;
            if (instruction_index_at(&editor->instructions, node->u.table_switch.default_target,
                                     &target))
                propagate(states, target, &work, editor->max_locals, editor->max_stack,
                          queue, count, &tail, &pending);
            const i4 entries = node->u.table_switch.high - node->u.table_switch.low + 1;
            for (i4 i = 0; i < entries; i++)
            {
                if (instruction_index_at(&editor->instructions,
                                         node->u.table_switch.targets[i], &target))
                    propagate(states, target, &work, editor->max_locals, editor->max_stack,
                              queue, count, &tail, &pending);
            }
        }
        else if (node->kind == OPERAND_LOOKUP_SWITCH)
        {
            u4 target = 0;
            if (instruction_index_at(&editor->instructions, node->u.lookup_switch.default_target,
                                     &target))
                propagate(states, target, &work, editor->max_locals, editor->max_stack,
                          queue, count, &tail, &pending);
            for (i4 i = 0; i < node->u.lookup_switch.count; i++)
            {
                if (instruction_index_at(&editor->instructions,
                                         node->u.lookup_switch.pairs[i].target, &target))
                    propagate(states, target, &work, editor->max_locals, editor->max_stack,
                              queue, count, &tail, &pending);
            }
        }
        else if (!terminal && index + 1u < count)
            propagate(states, index + 1u, &work, editor->max_locals,
                      editor->max_stack, queue, count, &tail, &pending);
    }

    free_states(states, count);
    free(queue);
    free(locals);
    free(stack);

    if (ambiguous)
        return CONSTRUCTOR_INIT_AMBIGUOUS;
    if (!found)
        return CONSTRUCTOR_INIT_MISSING;

    *out_offset = found_offset;
    return CONSTRUCTOR_INIT_FOUND;
}
