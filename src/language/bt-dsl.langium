grammar BtDsl

// ========================================================
// 1. Top Level Structure
// ========================================================

entry Program:
    (innerDocs+=INNER_DOC)*
    (imports+=ImportStmt)*
    (declarations+=DeclareStmt)*
    (globalVars+=GlobalVarDecl)*
    (trees+=TreeDef)*;

ImportStmt:
    'import' path=STRING;

DeclareStmt:
    (outerDocs+=OUTER_DOC)*
    'declare' category=ID name=ID '(' (ports+=DeclarePort (',' ports+=DeclarePort)*)? ')';

DeclarePort:
    (outerDocs+=OUTER_DOC)*
    direction=PortDirection? name=ID ':' typeName=ID;

// NodeCategory is now validated in the validator instead of the grammar
// Valid values: Action, Condition, Control, Decorator, SubTree

GlobalVarDecl:
    'var' name=ID ':' typeName=ID;

LocalVarDecl:
    'var' name=ID (':' typeName=ID)? ('=' initialValue=Expression)?;

// ========================================================
// 2. Tree Definition
// ========================================================

TreeDef:
    (outerDocs+=OUTER_DOC)*
    'Tree' name=ID '(' params=ParamList? ')' '{'
        (localVars+=LocalVarDecl)*
        body=NodeStmt
    '}';

ParamList:
    params+=ParamDecl (',' params+=ParamDecl)*;

ParamDecl:
    direction=PortDirection? name=ID (':' typeName=ID)?;

PortDirection returns string:
    'in' | 'out' | 'ref';

// ========================================================
// 3. Node Structure
// ========================================================

// Node name can refer to either a user-defined TreeDef or an external (manifest) node.
type NodeNameTarget = TreeDef | ManifestAction | ManifestCondition | ManifestControl | ManifestSubTree;

// Decorators in this DSL can only refer to manifest Decorator nodes.
type DecoratorNameTarget = ManifestDecorator;

// Virtual AST node types used for manifest-provided nodes.
// (Not part of the concrete syntax of .bt files; instances are provided via ScopeProvider.)
ManifestAction:
    'manifestAction' id=ID;

ManifestCondition:
    'manifestCondition' id=ID;

ManifestControl:
    'manifestControl' id=ID;

ManifestSubTree:
    'manifestSubTree' id=ID;

ManifestDecorator:
    'manifestDecorator' id=ID;

NodeStmt:
    (outerDocs+=OUTER_DOC)*
    (decorators+=Decorator)*
        nodeName=[NodeNameTarget:ID]
        (
                // Nodes with children: property block optional.
                // (Whether this is *semantically* allowed depends on manifest category; checked in Validator.)
                propertyBlock=PropertyBlock?
                childrenBlock=ChildrenBlock
            |
                // Nodes without children must have parentheses.
                propertyBlock=PropertyBlock
        );

Decorator:
    '@' name=[DecoratorNameTarget:ID] propertyBlock=PropertyBlock?;

PropertyBlock:
    '(' (args+=Argument (',' args+=Argument)*)? ')';

ChildrenBlock:
    '{' (children+=(NodeStmt | ExpressionStmt))* '}';

// ========================================================
// 4. Arguments & Values
// ========================================================

Argument:
    (name=ID ':')? value=ValueExpr;

ValueExpr:
    BlackboardRef | Literal;

// Union type for variable declarations
type NamedElement = GlobalVarDecl | LocalVarDecl | ParamDecl;

BlackboardRef:
    direction=PortDirection? varName=[NamedElement:ID];

Literal:
    StringLiteral | IntLiteral | FloatLiteral | BoolLiteral;

StringLiteral:
    value=STRING;

IntLiteral:
    value=INT;

FloatLiteral:
    value=FLOAT;

BoolLiteral:
    value?='true' | 'false';

// ========================================================
// 5. Expression System (for scripting support)
// ========================================================

// Expression statement in tree body (generates <Script> node)
ExpressionStmt:
    assignment=AssignmentExpr;

// Assignment: target = expr or target += expr etc.
AssignmentExpr:
    target=[NamedElement:ID] op=('=' | '+=' | '-=' | '*=' | '/=') value=Expression;

// Expression hierarchy with standard operator precedence
Expression:
    OrExpr;

OrExpr infers Expression:
    AndExpr ({infer BinaryExpr.left=current} op='||' right=AndExpr)*;

AndExpr infers Expression:
    BitwiseOrExpr ({infer BinaryExpr.left=current} op='&&' right=BitwiseOrExpr)*;

BitwiseOrExpr infers Expression:
    BitwiseAndExpr ({infer BinaryExpr.left=current} op='|' right=BitwiseAndExpr)*;

BitwiseAndExpr infers Expression:
    EqualityExpr ({infer BinaryExpr.left=current} op='&' right=EqualityExpr)*;

EqualityExpr infers Expression:
    ComparisonExpr ({infer BinaryExpr.left=current} op=('==' | '!=') right=ComparisonExpr)?;

ComparisonExpr infers Expression:
    AdditiveExpr ({infer BinaryExpr.left=current} op=('<' | '<=' | '>' | '>=') right=AdditiveExpr)?;

AdditiveExpr infers Expression:
    MultiplicativeExpr ({infer BinaryExpr.left=current} op=('+' | '-') right=MultiplicativeExpr)*;

MultiplicativeExpr infers Expression:
    UnaryExpr ({infer BinaryExpr.left=current} op=('*' | '/' | '%') right=UnaryExpr)*;

UnaryExpr infers Expression:
    {infer UnaryExpr} op=('!' | '-') operand=UnaryExpr
    | PrimaryExpr;

PrimaryExpr infers Expression:
    ParenExpr | LiteralExpr | VarRefExpr;

ParenExpr infers Expression:
    '(' Expression ')';

LiteralExpr:
    literal=Literal;

VarRefExpr:
    varRef=[NamedElement:ID];

// ========================================================
// 6. Lexical Rules (Terminals)
// ========================================================

hidden terminal WS: /\s+/;
terminal OUTER_DOC: /\/\/\/[^\n\r]*/;
terminal INNER_DOC: /\/\/![^\n\r]*/;
hidden terminal SL_COMMENT: /\/\/[^\n\r]*/;
hidden terminal ML_COMMENT: /\/\*[\s\S]*?\*\//;
terminal ID: /[a-zA-Z_][a-zA-Z0-9_]*/;
terminal STRING: /"([^"\\]|\\.)*"/;
terminal FLOAT: /-?[0-9]+\.[0-9]+/;
terminal INT: /-?[0-9]+/;
