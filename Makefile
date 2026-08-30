# kilix-media-sdk — aggregate build over the components present in this tree.
#
# Dependency ordering is explicit: components are built in the order listed
# here, and a component is never built by globbing.

COMPONENTS := kilix-acoustic-link

.PHONY: all test clean install manifest help

all:
	@set -e; for component in $(COMPONENTS); do \
	    echo "== build components/$$component"; \
	    $(MAKE) -C components/$$component; \
	done

test:
	@set -e; for component in $(COMPONENTS); do \
	    echo "== test components/$$component"; \
	    $(MAKE) -C components/$$component test; \
	done

install:
	@set -e; for component in $(COMPONENTS); do \
	    $(MAKE) -C components/$$component install; \
	done

clean:
	@set -e; for component in $(COMPONENTS); do \
	    $(MAKE) -C components/$$component clean; \
	done

# Print the component inventory the release manifest must match.
manifest:
	@for component in $(COMPONENTS); do \
	    printf '%s %s\n' "$$component" "$$(cat components/$$component/VERSION)"; \
	done

help:
	@echo "targets: all test install clean manifest"
