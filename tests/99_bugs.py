# https://github.com/blueloveTH/pocketpy/issues/37

mp = map(lambda x:  x**2, [1, 2, 3, 4, 5]  )
assert list(mp) == [1, 4, 9, 16, 25]


assert not 3>4

def f(x):
    if x>1:
        return 1

assert f(2) == 1
assert f(0) == None

a = [1, 2]
b = [3, 4]
assert a.append == a.append
assert a.append is not a.append
assert a.append is not b.append
assert a.append != b.append

inq = 0
if not inq:
    assert True
else:
    assert False

if inq is   not 1:
    assert True
if inq  is  not  0:
    assert False

assert pow(2,5000,2**59-1) == 17592186044416

def g(x):
    return x
def f(x):
    return x

assert (g(1), 2) == (1, 2)
assert (
    g(1),
    2
) == (1, 2)

assert f((
    g(1),
    2
)) == (1, 2)