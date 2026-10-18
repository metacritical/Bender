(import ./module :prefix "Q/" :only [hot] :export true)
(defn invoke [x] (Q/hot x))
(invoke 41)
