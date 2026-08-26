#include "instruction.h"

#include "../byte_stream.h"

#include <stdlib.h>
#include <string.h>

operand_kind instruction_kind_of(u1 opcode)
{
    switch (opcode)
    {
    case JVM_OPC_bipush:
    case JVM_OPC_sipush:
    case JVM_OPC_newarray:
        return OPERAND_IMMEDIATE;

    case JVM_OPC_iload:
    case JVM_OPC_lload:
    case JVM_OPC_fload:
    case JVM_OPC_dload:
    case JVM_OPC_aload:
    case JVM_OPC_istore:
    case JVM_OPC_lstore:
    case JVM_OPC_fstore:
    case JVM_OPC_dstore:
    case JVM_OPC_astore:
    case JVM_OPC_ret:
        return OPERAND_LOCAL;

    case JVM_OPC_iinc:
        return OPERAND_IINC;

    case JVM_OPC_ldc:
    case JVM_OPC_ldc_w:
    case JVM_OPC_ldc2_w:
        return OPERAND_CONSTANT;

    case JVM_OPC_getstatic:
    case JVM_OPC_putstatic:
    case JVM_OPC_getfield:
    case JVM_OPC_putfield:
    case JVM_OPC_invokevirtual:
    case JVM_OPC_invokespecial:
    case JVM_OPC_invokestatic:
    case JVM_OPC_new:
    case JVM_OPC_anewarray:
    case JVM_OPC_checkcast:
    case JVM_OPC_instanceof:
        return OPERAND_CP_INDEX;

    case JVM_OPC_invokeinterface:
        return OPERAND_INVOKE_INTERFACE;

    case JVM_OPC_invokedynamic:
        return OPERAND_INVOKE_DYNAMIC;

    case JVM_OPC_multianewarray:
        return OPERAND_MULTIANEWARRAY;

    case JVM_OPC_ifeq:
    case JVM_OPC_ifne:
    case JVM_OPC_iflt:
    case JVM_OPC_ifge:
    case JVM_OPC_ifgt:
    case JVM_OPC_ifle:
    case JVM_OPC_if_icmpeq:
    case JVM_OPC_if_icmpne:
    case JVM_OPC_if_icmplt:
    case JVM_OPC_if_icmpge:
    case JVM_OPC_if_icmpgt:
    case JVM_OPC_if_icmple:
    case JVM_OPC_if_acmpeq:
    case JVM_OPC_if_acmpne:
    case JVM_OPC_goto:
    case JVM_OPC_jsr:
    case JVM_OPC_ifnull:
    case JVM_OPC_ifnonnull:
        return OPERAND_BRANCH;

    case JVM_OPC_goto_w:
    case JVM_OPC_jsr_w:
        return OPERAND_BRANCH_WIDE;

    case JVM_OPC_tableswitch:
        return OPERAND_TABLE_SWITCH;

    case JVM_OPC_lookupswitch:
        return OPERAND_LOOKUP_SWITCH;

    default:
        return OPERAND_NONE;
    }
}

void instruction_free(instruction *node)
{
    if (node == NULL)
        return;
    if (node->kind == OPERAND_TABLE_SWITCH)
        free(node->u.table_switch.targets);
    else if (node->kind == OPERAND_LOOKUP_SWITCH)
        free(node->u.lookup_switch.pairs);
    memset(node, 0, sizeof(*node));
}

u4 instruction_length_at(const instruction *node, u4 offset)
{
    static const unsigned char lengths[JVM_OPC_MAX + 1] = JVM_OPCODE_LENGTH_INITIALIZER;

    switch (node->kind)
    {
    case OPERAND_LOCAL:
        return node->wide ? 4u : 2u;
    case OPERAND_IINC:
        return node->wide ? 6u : 3u;
    case OPERAND_TABLE_SWITCH:
    {
        const u4 padding = (4u - ((offset + 1u) % 4u)) % 4u;
        const u4 entries = (u4)(node->u.table_switch.high - node->u.table_switch.low + 1);
        return 1u + padding + 12u + entries * 4u;
    }
    case OPERAND_LOOKUP_SWITCH:
    {
        const u4 padding = (4u - ((offset + 1u) % 4u)) % 4u;
        return 1u + padding + 8u + (u4)node->u.lookup_switch.count * 8u;
    }
    default:
        return lengths[node->opcode];
    }
}

void instruction_list_init(instruction_list *list)
{
    list->items = NULL;
    list->count = 0;
    list->capacity = 0;
    list->code_length = 0;
}

void instruction_list_free(instruction_list *list)
{
    if (list->items != NULL)
    {
        for (u4 i = 0; i < list->count; i++)
            instruction_free(&list->items[i]);
        free(list->items);
    }
    instruction_list_init(list);
}

classfile_status instruction_list_reserve(instruction_list *list, u4 capacity)
{
    if (capacity <= list->capacity)
        return CLASSFILE_OK;

    instruction *grown = realloc(list->items, (size_t)capacity * sizeof(*grown));
    if (grown == NULL)
        return CLASSFILE_ERR_OUT_OF_MEMORY;

    memset(grown + list->capacity, 0, (size_t)(capacity - list->capacity) * sizeof(*grown));
    list->items = grown;
    list->capacity = capacity;
    return CLASSFILE_OK;
}

classfile_status instruction_list_append(instruction_list *list, instruction **out)
{
    if (list->count == list->capacity)
    {
        const u4 capacity = list->capacity ? list->capacity * 2u : 32u;
        const classfile_status status = instruction_list_reserve(list, capacity);
        if (status != CLASSFILE_OK)
            return status;
    }

    instruction *node = &list->items[list->count++];
    memset(node, 0, sizeof(*node));
    if (out != NULL)
        *out = node;
    return CLASSFILE_OK;
}

static i4 read_i4(byte_cursor *c) { return (i4)byte_cursor_u4(c); }
static i4 read_i2(byte_cursor *c) { return (i4)(int16_t)byte_cursor_u2(c); }
static i4 read_i1(byte_cursor *c) { return (i4)(int8_t)byte_cursor_u1(c); }

classfile_status instruction_list_parse(const u1 *code, u4 code_length, instruction_list *out)
{
    instruction_list_init(out);
    out->code_length = code_length;

    byte_cursor c;
    byte_cursor_init(&c, code, code_length);

    while (!byte_cursor_exhausted(&c) && byte_cursor_ok(&c))
    {
        const u4 offset = (u4)byte_cursor_offset(&c);

        instruction *node = NULL;
        classfile_status status = instruction_list_append(out, &node);
        if (status != CLASSFILE_OK)
        {
            instruction_list_free(out);
            return status;
        }

        node->offset = offset;
        node->opcode = byte_cursor_u1(&c);

        if (node->opcode == JVM_OPC_wide)
        {
            node->wide = true;
            node->opcode = byte_cursor_u1(&c);
        }

        node->kind = instruction_kind_of(node->opcode);
        if (node->wide && node->kind != OPERAND_LOCAL && node->kind != OPERAND_IINC)
        {
            instruction_list_free(out);
            return CLASSFILE_ERR_TRUNCATED;
        }

        switch (node->kind)
        {
        case OPERAND_NONE:
            break;

        case OPERAND_IMMEDIATE:
            node->u.immediate.value = node->opcode == JVM_OPC_sipush ? read_i2(&c) : read_i1(&c);
            break;

        case OPERAND_LOCAL:
            node->u.local.index = node->wide ? byte_cursor_u2(&c) : byte_cursor_u1(&c);
            break;

        case OPERAND_IINC:
            node->u.iinc.index = node->wide ? byte_cursor_u2(&c) : byte_cursor_u1(&c);
            node->u.iinc.constant = node->wide ? read_i2(&c) : read_i1(&c);
            break;

        case OPERAND_CONSTANT:
            node->u.constant.index = node->opcode == JVM_OPC_ldc ? byte_cursor_u1(&c)
                                                                 : byte_cursor_u2(&c);
            break;

        case OPERAND_CP_INDEX:
            node->u.cp.index = byte_cursor_u2(&c);
            break;

        case OPERAND_INVOKE_INTERFACE:
            node->u.invoke_interface.index = byte_cursor_u2(&c);
            node->u.invoke_interface.count = byte_cursor_u1(&c);
            node->u.invoke_interface.zero = byte_cursor_u1(&c);
            break;

        case OPERAND_INVOKE_DYNAMIC:
            node->u.invoke_dynamic.index = byte_cursor_u2(&c);
            node->u.invoke_dynamic.zero = byte_cursor_u2(&c);
            break;

        case OPERAND_MULTIANEWARRAY:
            node->u.multianewarray.index = byte_cursor_u2(&c);
            node->u.multianewarray.dimensions = byte_cursor_u1(&c);
            break;

        case OPERAND_BRANCH:
            node->u.branch.target = (i4)offset + read_i2(&c);
            break;

        case OPERAND_BRANCH_WIDE:
            node->u.branch.target = (i4)offset + read_i4(&c);
            break;

        case OPERAND_TABLE_SWITCH:
        {
            while (byte_cursor_offset(&c) % 4u != 0u && byte_cursor_ok(&c))
                (void)byte_cursor_u1(&c);

            node->u.table_switch.default_target = (i4)offset + read_i4(&c);
            node->u.table_switch.low = read_i4(&c);
            node->u.table_switch.high = read_i4(&c);

            if (!byte_cursor_ok(&c) || node->u.table_switch.high < node->u.table_switch.low)
            {
                instruction_list_free(out);
                return CLASSFILE_ERR_TRUNCATED;
            }

            const u4 entries = (u4)(node->u.table_switch.high - node->u.table_switch.low + 1);
            if ((size_t)entries * 4u > byte_cursor_remaining(&c))
            {
                instruction_list_free(out);
                return CLASSFILE_ERR_TRUNCATED;
            }

            node->u.table_switch.targets = malloc((size_t)entries * sizeof(i4));
            if (node->u.table_switch.targets == NULL)
            {
                instruction_list_free(out);
                return CLASSFILE_ERR_OUT_OF_MEMORY;
            }
            for (u4 i = 0; i < entries; i++)
                node->u.table_switch.targets[i] = (i4)offset + read_i4(&c);
            break;
        }

        case OPERAND_LOOKUP_SWITCH:
        {
            while (byte_cursor_offset(&c) % 4u != 0u && byte_cursor_ok(&c))
                (void)byte_cursor_u1(&c);

            node->u.lookup_switch.default_target = (i4)offset + read_i4(&c);
            node->u.lookup_switch.count = read_i4(&c);

            if (!byte_cursor_ok(&c) || node->u.lookup_switch.count < 0)
            {
                instruction_list_free(out);
                return CLASSFILE_ERR_TRUNCATED;
            }

            const u4 pairs = (u4)node->u.lookup_switch.count;
            if ((size_t)pairs * 8u > byte_cursor_remaining(&c))
            {
                instruction_list_free(out);
                return CLASSFILE_ERR_TRUNCATED;
            }

            node->u.lookup_switch.pairs = malloc((size_t)pairs * sizeof(switch_pair));
            if (pairs != 0 && node->u.lookup_switch.pairs == NULL)
            {
                instruction_list_free(out);
                return CLASSFILE_ERR_OUT_OF_MEMORY;
            }
            for (u4 i = 0; i < pairs; i++)
            {
                node->u.lookup_switch.pairs[i].match = read_i4(&c);
                node->u.lookup_switch.pairs[i].target = (i4)offset + read_i4(&c);
            }
            break;
        }
        }

        if (!byte_cursor_ok(&c))
        {
            instruction_list_free(out);
            return CLASSFILE_ERR_TRUNCATED;
        }
    }

    if (!byte_cursor_ok(&c))
    {
        instruction_list_free(out);
        return CLASSFILE_ERR_TRUNCATED;
    }

    return CLASSFILE_OK;
}

void instruction_list_recompute_offsets(instruction_list *list)
{
    for (;;)
    {
        u4 offset = 0;
        bool changed = false;

        for (u4 i = 0; i < list->count; i++)
        {
            if (list->items[i].offset != offset)
            {
                list->items[i].offset = offset;
                changed = true;
            }
            offset += instruction_length_at(&list->items[i], offset);
        }

        list->code_length = offset;
        if (!changed)
            return;
    }
}

classfile_status instruction_list_encode(const instruction_list *list, u1 **out, u4 *out_length)
{
    byte_buffer b;
    byte_buffer_init(&b);
    byte_buffer_reserve(&b, list->code_length ? list->code_length : 64u);

    for (u4 i = 0; i < list->count; i++)
    {
        const instruction *node = &list->items[i];
        const u4 offset = (u4)b.size;

        if (node->wide)
            byte_buffer_u1(&b, JVM_OPC_wide);
        byte_buffer_u1(&b, node->opcode);

        switch (node->kind)
        {
        case OPERAND_NONE:
            break;

        case OPERAND_IMMEDIATE:
            if (node->opcode == JVM_OPC_sipush)
                byte_buffer_u2(&b, (u2)node->u.immediate.value);
            else
                byte_buffer_u1(&b, (u1)node->u.immediate.value);
            break;

        case OPERAND_LOCAL:
            if (node->wide)
                byte_buffer_u2(&b, node->u.local.index);
            else
                byte_buffer_u1(&b, (u1)node->u.local.index);
            break;

        case OPERAND_IINC:
            if (node->wide)
            {
                byte_buffer_u2(&b, node->u.iinc.index);
                byte_buffer_u2(&b, (u2)node->u.iinc.constant);
            }
            else
            {
                byte_buffer_u1(&b, (u1)node->u.iinc.index);
                byte_buffer_u1(&b, (u1)node->u.iinc.constant);
            }
            break;

        case OPERAND_CONSTANT:
            if (node->opcode == JVM_OPC_ldc)
                byte_buffer_u1(&b, (u1)node->u.constant.index);
            else
                byte_buffer_u2(&b, node->u.constant.index);
            break;

        case OPERAND_CP_INDEX:
            byte_buffer_u2(&b, node->u.cp.index);
            break;

        case OPERAND_INVOKE_INTERFACE:
            byte_buffer_u2(&b, node->u.invoke_interface.index);
            byte_buffer_u1(&b, node->u.invoke_interface.count);
            byte_buffer_u1(&b, node->u.invoke_interface.zero);
            break;

        case OPERAND_INVOKE_DYNAMIC:
            byte_buffer_u2(&b, node->u.invoke_dynamic.index);
            byte_buffer_u2(&b, node->u.invoke_dynamic.zero);
            break;

        case OPERAND_MULTIANEWARRAY:
            byte_buffer_u2(&b, node->u.multianewarray.index);
            byte_buffer_u1(&b, node->u.multianewarray.dimensions);
            break;

        case OPERAND_BRANCH:
            byte_buffer_u2(&b, (u2)(node->u.branch.target - (i4)offset));
            break;

        case OPERAND_BRANCH_WIDE:
            byte_buffer_u4(&b, (u4)(node->u.branch.target - (i4)offset));
            break;

        case OPERAND_TABLE_SWITCH:
        {
            while (b.size % 4u != 0u)
                byte_buffer_u1(&b, 0);
            byte_buffer_u4(&b, (u4)(node->u.table_switch.default_target - (i4)offset));
            byte_buffer_u4(&b, (u4)node->u.table_switch.low);
            byte_buffer_u4(&b, (u4)node->u.table_switch.high);
            const u4 entries = (u4)(node->u.table_switch.high - node->u.table_switch.low + 1);
            for (u4 k = 0; k < entries; k++)
                byte_buffer_u4(&b, (u4)(node->u.table_switch.targets[k] - (i4)offset));
            break;
        }

        case OPERAND_LOOKUP_SWITCH:
        {
            while (b.size % 4u != 0u)
                byte_buffer_u1(&b, 0);
            byte_buffer_u4(&b, (u4)(node->u.lookup_switch.default_target - (i4)offset));
            byte_buffer_u4(&b, (u4)node->u.lookup_switch.count);
            for (i4 k = 0; k < node->u.lookup_switch.count; k++)
            {
                byte_buffer_u4(&b, (u4)node->u.lookup_switch.pairs[k].match);
                byte_buffer_u4(&b, (u4)(node->u.lookup_switch.pairs[k].target - (i4)offset));
            }
            break;
        }
        }
    }

    if (!byte_buffer_ok(&b))
    {
        byte_buffer_free(&b);
        return CLASSFILE_ERR_OUT_OF_MEMORY;
    }

    size_t size = 0;
    *out = byte_buffer_release(&b, &size);
    *out_length = (u4)size;
    return CLASSFILE_OK;
}

const instruction *instruction_list_at_offset(const instruction_list *list, u4 offset)
{
    for (u4 i = 0; i < list->count; i++)
    {
        if (list->items[i].offset == offset)
            return &list->items[i];
    }
    return NULL;
}
