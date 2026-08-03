const nameCollator = new Intl.Collator(undefined, {
  sensitivity: "base",
  numeric: true,
});

export function compareNames(left: string, right: string): number {
  return nameCollator.compare(left, right);
}
