# Expanded binding's initializer is deliberately not the named child.
# A different, reachable function happens to share the binding name.
(def hot {:hint {:returns :long}} (fn mismatch [] 3))
(def use (fn hot [] 4))
(use)
