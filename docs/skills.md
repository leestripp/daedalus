# Agent Skills - Anthropic's Open Standard for AI Capabilities

## Overview

**Agent Skills** is an open standard introduced by Anthropic in December 2025 for packaging reusable capabilities, domain expertise, and specialized workflows for AI agents. Following the same approach that made MCP (Model Context Protocol) an industry standard, Anthropic released the specification and SDK at [agentskills.io](https://agentskills.io) for any AI provider or tool to adopt.

Agent Skills are portable packages of instructions, scripts, and resources that agents can discover and use to perform tasks more accurately and efficiently.

---

## Why Agent Skills?

AI agents need to handle diverse, specialized tasks. Rather than relying solely on prompts or building custom integrations, Agent Skills provide a standardized way to:

- **Package domain expertise** - Share knowledge about specific domains (e.g., coding frameworks, compliance requirements)
- **Define reusable workflows** - Encapsulate best practices for recurring tasks
- **Extend agent capabilities** - Add new knowledge without modifying core agent code
- **Enable cross-platform compatibility** - Use the same skills across different AI tools and providers

---

## Skill Structure

A **skill is a directory** containing, at minimum, a `SKILL.md` file. Optional subdirectories can hold scripts, references, and assets.

```
skill-name/
├── SKILL.md              # Required: Metadata + Instructions (YAML frontmatter + Markdown body)
├── scripts/             # Optional: Executable scripts the agent can run
├── references/           # Optional: Reference documentation
├── resources/            # Optional: Asset files (templates, configs, etc.)
└── assets/               # Optional: Binary or data files
```

### SKILL.md Format

The `SKILL.md` file is the core component of any agent skill. It consists of two parts:

1. **YAML Frontmatter** - Metadata that helps agents discover and activate the skill
2. **Markdown Body** - Instructions that guide the agent's behavior when activated

```markdown
---
name: skill-name
description: Brief trigger condition for when this skill should activate
version: 1.0.0
---

# Skill Title

Detailed instructions for how the agent should perform this task...

## Step 1: Do This
First step description...

## Step 2: Do That
Second step description...

## Examples
Example usage or code snippets...
```

### YAML Frontmatter Fields

| Field | Required | Description |
|-------|----------|-------------|
| `name` | Yes | Unique identifier for the skill (kebab-case recommended) |
| `description` | Yes | **Trigger condition** - describes when this skill should activate. Used by the LLM to decide if the skill is relevant. |
| `version` | No | Semantic version (e.g., `1.0.0`) |
| `license` | No | License for the skill (e.g., `MIT`, `Apache-2.0`) |
| `compatibility` | No | Compatible runtimes or tools |

### The `description` Field - Trigger Conditions

The `description` field is crucial - it is **not** for human-readable documentation. It is the **trigger condition** the agent uses when deciding whether to activate this skill.

**Good trigger descriptions:**
```
description: "Analyzes C++ code for memory leaks and suggests fixes"
description: "Handles Git merge conflicts and creates proper commit messages"
description: "Writes unit tests for Python functions using pytest"
```

**Bad trigger descriptions (too generic):**
```
description: "This skill helps with programming"  # Too vague
description: "General coding assistance"         # Won't trigger reliably
```

---

## How Skills Work

### Discovery Phase

When an agent starts or when processing a task, it can browse available skills by reading their `SKILL.md` frontmatter. The agent evaluates each skill's `description` against the current task context.

### Loading Phase

The agent **loads only the SKILL.md frontmatter** initially. This keeps the context window efficient. The skill activates when the LLM determines the task matches the trigger condition.

### Activation Phase

When activated:
1. The full `SKILL.md` body (instructions) is loaded into context
2. If needed, `scripts/`, `references/`, or `assets/` directories are made available
3. The agent follows the instructions to complete the task

### Execution Phase

The agent executes the task using its existing tools (execute_bash, read_file, etc.) following the skill's instructions.

---

## Skills vs MCP - Different Problems, Different Solutions

| Aspect | **Skills** | **MCP** |
|--------|-----------|---------|
| **Purpose** | Teaches agents **how** to do things | Gives agents **new capabilities** |
| **What they add** | Static documentation and instructions | Dynamic tool definitions |
| **Format** | Markdown files with YAML frontmatter | JSON-RPC protocol over stdio/HTTP |
| **Loading** | On-demand when relevant | Persistent tool definitions |
| **Scope** | Best practices, workflows, domain knowledge | External data sources, APIs, services |
| **Analogy** | Recipe cards | Kitchen appliances |

**Use Skills when you need to:**
- Share best practices for specific frameworks or languages
- Define consistent workflows (code review, testing, deployment)
- Package domain expertise (compliance, security scanning)
- Create reusable instruction templates

**Use MCP when you need to:**
- Connect to databases, APIs, or external services
- Provide real-time data access
- Enable file system operations
- Integrate with third-party tools

**They complement each other:** Skills can reference MCP servers, and MCP servers can load Skills dynamically.

---

## SDK and Implementation

### Official Resources

- **Specification**: [agentskills.io/specification](https://agentskills.io/specification)
- **Quick Start Guide**: [agentskills.io/skill-creation/quickstart](https://agentskills.io/skill-creation/quickstart)
- **Client Implementation**: [agentskills.io/client-implementation](https://agentskills.io/client-implementation)
- **Official SDK**: Available at agentskills.io

### Key SDK Functions

The SDK provides functions for:

1. **Skill Discovery**
   - `list_skills(directory)` - List all skills in a directory
   - `get_skill_metadata(skill_path)` - Read SKILL.md frontmatter
   - `search_skills(query)` - Search skills by trigger conditions

2. **Skill Loading**
   - `load_skill(skill_path)` - Load full SKILL.md content
   - `get_skill_resources(skill_path)` - Access scripts/references/assets
   - `validate_skill(skill_path)` - Check skill format compliance

3. **Skill Execution**
   - `execute_skill_script(skill_path, script_name)` - Run skill scripts
   - `get_skill_context(skill_path)` - Get formatted instructions

### Skill Directories Convention

| Path | Description |
|------|-------------|
| `.agents/skills/` | Project-local skills |
| `~/.daedalus/skills/` | User-level skills (daedalus convention) |
| `~/.claude/skills/` | Claude Code convention |
| `/usr/local/share/agent-skills/` | System-wide skills |

### Example: A Memory Leak Detection Skill

daedalus ships with a memory-leak skill that uses Valgrind:

```
~/.daedalus/skills/memory-leak/
└── SKILL.md
```

SKILL.md example:
```markdown
---
name: memory-leak
description: "Detects memory leaks in C/C++ and other binaries using Valgrind Memcheck"
version: 1.0.0
---

# Memory Leak Detection Skill

This skill uses Valgrind (Memcheck) to detect memory leaks and memory errors in binaries.

## Workflow

### Basic Memory Leak Check

Run valgrind with default leak checking:

```bash
valgrind --leak-check=full --log-file=valgrind.log ./your_executable arg1 arg2
```

### Parse Exit Code

```bash
valgrind [options] ./binary [args]
if [ $? -ne 0 ] ; then
    echo "Memory errors or leaks found!"
    cat valgrind.log
fi
```
```

---

## Example: A C++ Code Review Skill

### Directory Structure

```
cpp-code-review/
├── SKILL.md
├── scripts/
│   ├── run_static_analysis.sh
│   └── generate_report.py
└── references/
    └── cpp-guidelines.md
```

### SKILL.md

```markdown
---
name: cpp-code-review
description: "Reviews C++ code for memory leaks, undefined behavior, and concurrency issues"
version: 1.0.0
---

# C++ Code Review Skill

This skill performs comprehensive code reviews for C++ codebases.

## Pre-Review Checklist

1. [ ] Verify build system configuration
2. [ ] Check compiler flags and warnings
3. [ ] Review header dependencies

## Review Areas

### Memory Safety
- Use-after-free detection
- Memory leak patterns (missing delete, smart pointer usage)
- Buffer overflow vulnerabilities

### Concurrency
- Race conditions in multi-threaded code
- Deadlock-prone patterns
- Thread-safe resource management

### Modern C++ Best Practices
- Prefer `std::make_unique` and `std::make_shared`
- Use `const` correctness
- Avoid raw pointers where smart pointers suffice

## Running Analysis

Use the provided scripts:

```bash
./scripts/run_static_analysis.sh <source_file>
```

## Output Format

Generate reports in markdown format with:
- Severity levels (Critical, Warning, Info)
- Line references
- Suggested fixes
```

---

## Adoption and Ecosystem

Agent Skills has been adopted by major companies:

- **Microsoft** - GitHub Copilot in VS Code
- **OpenAI** - ChatGPT and development tools
- **Atlassian** - Jira and Confluence integrations
- **Figma** - Design workflow integrations
- **Anthropic** - Claude Code, Claude Desktop, Cline

---

## Best Practices

1. **Write Clear Trigger Descriptions**
   - Be specific about when a skill should activate
   - Match terminology users would naturally use

2. **Keep Skills Focused**
   - One skill = one capability or workflow
   - Better to have many small skills than one monolithic skill

3. **Use Scripts Wisely**
   - Scripts should do one thing well
   - Document expected inputs and outputs

4. **Version Your Skills**
   - Use semantic versioning
   - Include changelog in SKILL.md or separate CHANGELOG.md

5. **Test Your Skills**
   - Verify skills trigger in appropriate contexts
   - Test scripts across different environments

---

## References

- [agentskills.io - Official Specification](https://agentskills.io/specification)
- [Anthropic Agent Skills Overview](https://platform.claude.com/docs/en/agents-and-tools/agent-skills/overview)
- [The Complete Guide to Building Skills for Claude (PDF)](https://resources.anthropic.com/hubfs/The-Complete-Guide-to-Building-Skill-for-Claude.pdf)
- [Building Effective AI Agents - Anthropic Research](https://www.anthropic.com/research/building-effective-agents)
- [Model Context Protocol (MCP)](https://modelcontextprotocol.io/docs/getting-started/intro)
