// generated from test/fixtures/testing-dev-optional-flags
module.exports = t => {
  const path = t.testdir({
  "package.json": JSON.stringify({
    "name": "@isaacs/testing-dev-optional-flags",
    "version": "1.0.0",
    "description": "a package for testing dev, optional, and devOptional flags",
    "devDependencies": {
      "abbrev": "^1.1.1",
      "once": "^1.4.0"
    },
    "optionalDependencies": {
      "own-or": "^1.0.0",
      "wrappy": "^1.0.2"
    },
    "dependencies": {
      "wrappy": "^1.0.2",
      "own-or": "^1.0.0"
    }
  })
})
  return path
}
