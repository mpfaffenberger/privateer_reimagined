"""reskin_nodes.py — tiny Blender shader-node builders shared by the reskin
tools. Operands are sockets (linked) or constants (set as defaults), which
keeps node-graph code close to the maths it expresses."""

import bpy


def node_tree_of(mat):
    """Materials always have node trees from Blender 5.0; `use_nodes` is
    deprecated there but still required on 4.x."""
    if bpy.app.version < (5, 0, 0):
        mat.use_nodes = True
    return mat.node_tree


def add_node(tree, kind, location=(0, 0), **props):
    n = tree.nodes.new(kind)
    n.location = location
    for key, value in props.items():
        setattr(n, key, value)
    return n


def math_op(tree, op, a=None, b=None, c=None, location=(0, 0)):
    """Math node; each operand is either a socket (linked) or a float."""
    n = add_node(tree, "ShaderNodeMath", location, operation=op)
    for idx, operand in enumerate((a, b, c)):
        if operand is None:
            continue
        if isinstance(operand, (int, float)):
            n.inputs[idx].default_value = float(operand)
        else:
            tree.links.new(operand, n.inputs[idx])
    return n.outputs[0]


def vec_op(tree, op, a, b=None, location=(0, 0)):
    """VectorMath node; operands are sockets or 3-tuples. Returns the node
    (DOT_PRODUCT results live on outputs['Value'], others on outputs[0])."""
    n = add_node(tree, "ShaderNodeVectorMath", location, operation=op)
    for idx, operand in enumerate((a, b)):
        if operand is None:
            continue
        if isinstance(operand, tuple):
            n.inputs[idx].default_value = operand
        else:
            tree.links.new(operand, n.inputs[idx])
    return n


def smoothstep(tree, value, lo, hi, location=(0, 0)):
    n = add_node(tree, "ShaderNodeMapRange", location, interpolation_type="SMOOTHSTEP")
    tree.links.new(value, n.inputs["Value"])
    n.inputs["From Min"].default_value = lo
    n.inputs["From Max"].default_value = hi
    return n.outputs["Result"]
