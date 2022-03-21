const { definitions } = require('../lib/utils/config/index.js')
const usageFn = require('../lib/utils/usage.js')
const { writeFileSync, readFileSync } = require('fs')
const { resolve } = require('path')

const configDoc = process.argv[2]
const commandFile = process.argv[3]

const TAGS = {
  CONFIG: {
    START: '<!-- AUTOGENERATED CONFIG DESCRIPTIONS START -->',
    END: '<!-- AUTOGENERATED CONFIG DESCRIPTIONS END -->',
  },
  USAGE: {
    START: '<!-- AUTOGENERATED USAGE DESCRIPTIONS START -->',
    END: '<!-- AUTOGENERATED USAGE DESCRIPTIONS END -->',
  },
}

// Note: commands without params skip this whole process.
const {
  params,
  usage,
} = require(resolve(commandFile))

const describeAll = (content) =>
  content.map(name => definitions[name].describe()).join(
    '\n\n<!-- automatically generated, do not edit manually -->\n' +
      '<!-- see lib/utils/config/definitions.js -->\n\n'
  )

const describeUsage = ({ usage }) => {
  const synopsis = []

  // Grab the command name from the *.md filename
  // NOTE: We cannot use the name property command file because in the case of
  // `npx` the file being used is `lib/commands/exec.js`
  const commandName = configDoc.split('/').pop().split('.')[0].replace('npm-', '')
  synopsis.push('\n```bash')

  if (commandName) {
    // special case for `npx`:
    // `npx` is not technically a command in and of itself,
    // so it just needs the usage and parameters of npm exec, and none of the aliases
    if (commandName === 'npx') {
      synopsis.push(usage.map(usageInfo => `npx ${usageInfo}`).join('\n'))
    } else {
      const baseCommand = `npm ${commandName}`
      if (!usage) {
        synopsis.push(baseCommand)
      } else {
        synopsis.push(usage.map(usageInfo => `${baseCommand} ${usageInfo}`).join('\n'))
      }

      const aliases = usageFn(commandName, '').trim()
      if (aliases) {
        synopsis.push(`\n${aliases}`)
      }
    }
  } else {
    console.error(`could not determine command name from ${commandFile}`)
  }

  synopsis.push('```')
  return synopsis.join('\n')
}

const addBetweenTags = (
  doc,
  startTag,
  endTag,
  body,
  sourceFilepath = 'lib/utils/config/definitions.js') => {
  const startSplit = doc.split(startTag)

  if (startSplit.length !== 2) {
    throw new Error('Did not find exactly one start tag')
  }

  const endSplit = startSplit[1].split(endTag)
  if (endSplit.length !== 2) {
    throw new Error('Did not find exactly one end tag')
  }

  return [
    startSplit[0],
    startTag,
    '\n<!-- automatically generated, do not edit manually -->\n' +
      '<!-- see ' + sourceFilepath + ' -->\n',
    body,
    '\n\n<!-- automatically generated, do not edit manually -->\n' +
      '<!-- see ' + sourceFilepath + ' -->',
    '\n\n',
    endTag,
    endSplit[1],
  ].join('')
}

const addDescriptions = doc =>
  addBetweenTags(doc, TAGS.CONFIG.START, TAGS.CONFIG.END, describeAll(params))

const addUsageDescriptions = doc =>
  addBetweenTags(doc, TAGS.USAGE.START, TAGS.USAGE.END,
    describeUsage({ usage }),
    commandFile
  )

try {
  // always write SOMETHING so that Make sees the file is up to date.
  const doc = readFileSync(configDoc, 'utf8')
  const hasTag = doc.includes(TAGS.CONFIG.START)
  const hasUsageTag = doc.includes(TAGS.USAGE.START)

  if (params && params.length) {
    let newDoc = hasTag ? addDescriptions(doc) : doc
    newDoc = hasUsageTag ? addUsageDescriptions(newDoc) : newDoc

    if (!hasTag) {
      console.error('WARNING: did not find config description section', configDoc)
    }

    if ((usage && usage.length) && !hasUsageTag) {
      console.error('WARNING: did not find usage description section', configDoc)
    }
    writeFileSync(configDoc, newDoc)
  }
} catch (err) {
  console.error(`WARNING: file cannot be open: ${configDoc}`)
  console.error(err)
}