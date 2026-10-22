::  mesa-echo: acks every poke; the interop test suspends and restarts it
::
/+  default-agent
^-  agent:gall
|_  =bowl:gall
+*  this  .
    def   ~(. (default-agent this %|) bowl)
++  on-init   `this
++  on-save   !>(~)
++  on-load   |=(vase `this)
++  on-poke
  |=  [=mark =vase]
  ~&  [%mesa-echo-poke src.bowl q.vase]
  `this
++  on-watch  on-watch:def
++  on-leave  on-leave:def
++  on-peek   on-peek:def
++  on-agent  on-agent:def
++  on-arvo   on-arvo:def
++  on-fail   on-fail:def
--
