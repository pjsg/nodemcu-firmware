# Freezer Module

This describes how the freezer module works. The model is of a freezer. You can `freeze` functions and they stay in the freezer. You can `defrost` the freezer which will eliminate all the contents on the next platform reboot.

The core API is `freezer.freeze` which takes a single argument (a function) and it returns a function that behaves in exactly the same way but uses much less RAM. 

The freezer module allocates a 64k (aligned) chunk of flash memory which is then used to store structures that are copied from RAM. The flash area is searched to see if the structure is already present in the flash area. If it is found, then no new flash is consumed. A number of different structures types are copied into the flash area. One wrinkle is that copying string objects into flash causes a problem as there can now be two copies of a particular string -- one in flash and one in ram. This prevents the string equality optimization of just comparing pointers.

Once there is no more flash area present, a flag is set such that the flash area is cleared on the next boot. This takes care of the development case where different versions of the same code are being loaded.



The objects that are copied:

* TString: note that this causes the problem of not being able to use pointer equality for equality testing

* Proto: Save the code, the source name (TString), the upvalue names (TString), the upvalue name vector, the local variable names (TString), the local variable name vector, string constants (TString), constant vector, the Proto block itself. It isn't quite that simple as you can only copy the vectors into flash if all their contents are fixed.

* Closure: freeze the Proto block and recursively call any inner closures.

You have to be careful reading from the flash after writing to it. The CPU has some amount of cache that means that you can read back old values. There is code to try and detect this case.

Some code like:

```
dofile = function (f)
   return freezer.freeze(loadfile(f))()
   end
```

arranges for all code loaded through dofile to be frozen into flash if possible. 

If you lose all references to a frozen function, then all of its RAM is GCed as today. The flash (obviously) remains behind (but would be resused if the same function was loaded again). The nodemcu pattern of using `dofile` to load functionality for a short time is no longer as important. In many cases it can be replaced by `require`. The benefit being much hfaster access on the second (and subsequent) invocations.


## Issues


* There is still a fair amount of RAM not saved. It is unclear whether it can be saved or not.

* If the flash area fills up, then it doesn't do the expected freezing (but it still returns a working function). The bad news is that you probably run out of memory. The good news is that after the platform reboots, the flash is cleared (because we ran out) and now all the code probably fits.

* The lack of nodemcu test cases makes me worried that all the cases are covered.

* Byte access to the flash is very slow. There probably needs to be work on the base code to make sure that these are minimized. It isn't a problem in the freezer code, but elsewhere as things start to access the flash directly.

