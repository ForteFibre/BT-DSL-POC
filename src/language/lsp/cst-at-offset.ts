import { CstUtils } from "langium";
import type { CstNode } from "langium";

export interface LeafAndAssignment {
  /** The leaf (token) node at the given offset. Used for text/range. */
  leaf: CstNode;
  /** The closest CST node (walking up parents) whose grammarSource is an Assignment. */
  assignment?: { node: CstNode; feature: string };
}

/**
 * Langium's `findDeclarationNodeAtOffset` is geared towards *declarations*.
 * For IDE features (hover/definition/completion) we need the leaf token at the cursor,
 * plus the surrounding assignment feature name (e.g. `nodeName`, `name`, `varName`).
 */
export function findLeafAndAssignmentAtOffset(
  root: CstNode,
  offset: number
): LeafAndAssignment | undefined {
  const leaf = CstUtils.findLeafNodeAtOffset(root, offset);
  if (!leaf) return undefined;

  let current: CstNode | undefined = leaf;
  while (current) {
    const feature = getAssignmentFeature(current.grammarSource);
    if (feature) {
      return { leaf, assignment: { node: current, feature } };
    }
    current = current.parent;
  }

  return { leaf };
}

function getAssignmentFeature(grammarSource: unknown): string | undefined {
  const gs = grammarSource as { $type?: string; feature?: string } | undefined;
  if (gs?.$type === "Assignment") {
    return gs.feature;
  }
  return undefined;
}
