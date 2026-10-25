nevr.hook("test.add", {
  post = function(h)
    h:set("a", 1)
  end,
})
