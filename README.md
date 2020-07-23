# EJSON - an extended-JSON format

EJSON is a dynamically-typed, Turing-incomplete language that sits on-top of JSON. It is domain-specific and can only produce data representation compatible with JSON.


## Syntax

An EJSON document consists of a series of **assignments** followed by an **expression**. The expression is what the document is based on and may make reference to previously made expressions. All JSON is valid EJSON. All EJSON documents can produce JSON that would parse in an identical way i.e. you cannot produce an EJSON document which will not expand to valid JSON.

    document = { assignment }, expression

## Assignments

Assignments give a name to an expression.

    assignment = "define", whitespace, identifier, "=", expression, ";"

## Expressions

There are 7 primtive types in EJSON:

* dictionaries
* lists
* strings
* real numbers
* null
* booleans (true/false)
* integers
* functions

All expressions evaluate to one of these types. The first six of these types correspond to the regular JSON types. Integers will automatically be promoted to reals. The main document expression must not expand to a function type.

    expression = identifier
        | "(", expression, ")"
        | expression, "+", expression
        | dict
        | list
        | string
        | boolean
        | numeric-literal
        | null
        | function
        | function-call
        | builtin
        | range-expression

    dict = "{", [ expression, ":", expression { ",", expression, ":", expression } ], "}"

    list = "[", [ expression, { ",", expression } ], "]"

    string = '"', characters, '"'

    boolean = "true" | "false"

    null = "null"

    function = "func", argument-list, expression

    function-call = "call", expression, expression

    builtin = "range", expression
        | "access", expression, expression
        | "map", expression, expression

## Operator precedence

| Token | Operator     | Precedence | Description              |
| ----- | ------------ | ---------- | ------------------------ |
| or    | Left Binary  | 1          | Logical or               |
| and   | Left Binary  | 2          | Logical and              |
| not   | Unary        | 3          | Logical not              |
| ==    | Left Binary  | 4          | Equal to                 |
| !=    | Left Binary  | 4          | Not equal to             |
| >     | Left Binary  | 5          | Greater than             |
| >=    | Left Binary  | 5          | Greater than or equal to |
| <     | Left Binary  | 5          | Less than                |
| <=    | Left Binary  | 5          | Less than or equal to    |
| \|    | Left Binary  | 6          | Bitwise or               |
| &     | Left Binary  | 7          | Bitwise and              |
| +     | Left Binary  | 8          | Addition                 |
| -     | Left Binary  | 8          | Subtraction              |
| *     | Left Binary  | 9          | Multiplication           |
| /     | Left Binary  | 9          | Division                 |
| %     | Left Binary  | 9          | Modulo                   |
| -     | Unary        | 10         | Negation                 |
| ^     | Right Binary | 11         | Exponentiation           |

## Range expression

    range-expression = "range", expression

The `range-expression` always evaluates to a `list` primitive type. The expression argument is an argument list containing one, two or three integer arguments.

### Single argument

In the case of single argument, the value represents the number of elements in the list beginning with zero and incrementing by one for each element.

    > range [4]
    [0, 1, 2, 3]

### Two arguments

Two arguments provides the values of first and last element.

    > range [1, 3]
    [1, 2, 3]
    > range [1, -2]
    [1, 0, -1, -2]

### Three arguments

Three arguments provides the value of the first element, step size and largest permitted value for the last element.

    > range [1, 2, 8]
    [1, 3, 5, 7]
    > range [1, -3, -8]
    [1, -2, -5, -8]

It is not permitted for the step-size argument to have a sign that does not move the values towards the target. i.e. the following is invalid:

    > range [1, -1, 2]
    invalid range

## Function and Function-call expressions

    function-expression = "func", function-arguments, expression
    function-arguments = "[", [ identifier, { ",", identifier } ], "]"

A `function-expression` takes two arguments, a `function-arguments` definition and an expression. It evaluates to a `function` type. As this type does not exist in JSON, it can only ever be used as an intermediate in obtaining a valid value. The argument list contains `identifiers` which can be used in the expression. The identifiers must not overlap with any `define`'ed variables or identifiers in the scope of the containing function.

The following contains a function which takes no arguments and returns 10, a function which takes two arguments and multiplies them and a function which returns a dictionary containing the argument.

    > func [] 10
    > func [x, y] x * y
    > func [z] {"ValueOfZ": z}

Because functions are a first-class object, a function can also return a function. For example, the following function returns a function that takes no arguments and returns a scalar that was the value of x multiplied by three:

    > func [x] func [] x*3

The following are invalid:

    > define x=1; func [x] 11*x
    function has a parameter overlapping with a defined variable
    > func [x, y] func [x] x*y
    function has a parameter overlapping in the scope of the calling function

A `function-call-expression` expression is defined as follows:

    function-call-expression = "call", expression, expression

The first argument must expand to a `function-expression` and the second must expand to a `list-expression` which provides values for the arguments of the given function and evaluates its expression.

    > call func [x, y] x*y [5, 7]
    35

## Format expression

    format-expression = "format", expression

A `format-expression` takes a single argument which must evaluate to a `list` containing at least one value. The first value must evaluate to a `string`. The string is a C _printf()_ style format specifier supporting %d (integer) and %s (string) types.

    > format ["I am %d, you are %03d, I have a %s"] [10, 11, "cat"]
    "I am 10, you are 011, I have a cat"

## Access expression

    access-expression = "access", expression, expression

A `access-expression` takes two arguments: either a `list` and an `integer` or a `dictionary` and a `string`. If the first argument is a `list`, the expression evaluates to the element of the list at the zero-based index of the given integer. It is an error condition for the list index to be out of range. If the first argument is a `dictionary`, the expression evaluates to the value with the given key. It is an error condition for the key to not exist.

    > access ["cat", "dog", "wolf"] 1
    "dog"
    > access range [10] 4
    4


## Map expressions

    map-expression = "map", expression, expression

A `map-expression` takes two arguments: a single-parameter `function` and a `list`. The expression always expands to a `list` of the same length as the input list argument. The resulting list contains the value of the function when evaluated with each element of the expression.

    > map func [x] x*x [1, 2, 3]
    [1, 4, 9]

