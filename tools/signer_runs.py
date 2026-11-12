"""The runs of the release-signer's `sign-manifest.yml` that a publish must not overlap (#1978).

The Quest release timer and the rollback tool both dispatch that workflow, and both read the same rule: a run that is not
`completed` (queued or in progress) counts as active. One definition here, so the two tools cannot drift apart.
"""
import json

SIGNER_SLUG = "thesprockee/release-signer"
WORKFLOW = "sign-manifest.yml"
RUN_LIST_ARGS = ["run", "list", "--workflow", WORKFLOW, "-R", SIGNER_SLUG, "--json", "status,databaseId", "--limit", "30"]


def unfinished_runs(listing: str) -> list[str]:
    """The runs in a `gh run list --json status,databaseId` answer that are not completed, as `<id> (<status>)`.

    Raises ValueError, KeyError or TypeError when the answer is not that listing; the caller turns that into its own failure.
    """
    return [f"{run['databaseId']} ({run['status']})" for run in json.loads(listing) if run["status"] != "completed"]
