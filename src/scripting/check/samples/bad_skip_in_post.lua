nevr.hook("test.add", {
  post = function(h)
    h:skip()
  end,
})
