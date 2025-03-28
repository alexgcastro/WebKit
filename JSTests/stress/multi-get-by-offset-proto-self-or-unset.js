function foo(o, start) {
    var result = 0;
    for (var i = 0; i < 100; ++i)
        result += o.f;
    return result;
}

noInline(foo);


var p = {};
p.f = 42;
var o = Object.create(p);

var q = {}
q.f = 42;

var f = {};

for (var i = 0; i < testLoopCount; ++i)
    o.f = i;
o.f = 42;

for (var i = 0; i < testLoopCount; ++i) {
    if (i % 100 === 0) {
        let result = foo(q)
        if (result !== 4200)
            throw new Error("bad result: " + result);
    }

    if (foo(o) !== 4200)
        throw new Error("bad result: " + result);
    var result = foo(f);
    if (!Number.isNaN(result))
        throw new Error("bad result: " + result);
}

var q = {};
q.f = 43;
var result = foo(q);
if (result != 100 * 43)
    throw "Error: bad result at end: " + result;
